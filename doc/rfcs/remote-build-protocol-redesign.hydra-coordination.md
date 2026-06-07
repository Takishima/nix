# Draft post: serve protocol diagnostic core — Hydra coordination

| | |
|------------------|------------------------------------------------|
| **Status**       | Draft for posting (Hydra ↔ Nix coordination thread) |
| **Audience**     | Hydra maintainers (esp. `hydra-queue-runner`), Nix serve-protocol maintainers |
| **Realizes**     | validation plan [Workstream D / D4](./remote-build-protocol-redesign.validation.md); decisions [Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51) |
| **Goal of thread** | a *named* Hydra maintainer signs off on the serve diagnostic core field set + byte order, and a `hydra-queue-runner` branch consumes it — the two blocking serve diagnostic-core freeze criteria |

> This is the **opening post** for the coordination thread the RFC deliberately
> left out of its own scope (RFC §10 Q4: "the Hydra coordination *thread* itself
> remains out of scope for this document"). Everything technical is already
> decided on the Nix side; what is needed here is Hydra-side review and a working
> consumer branch. Copy/adapt this into the actual discussion (GitHub
> Discussions / Discourse / matrix) when posting.

---

## TL;DR — what we're asking Hydra for

We want to add a small, **additive, version-gated** diagnostic surface to the
**serve protocol** (the `nix-store --serve` / `BasicClientConnection` wire that
`hydra-queue-runner` speaks) so that:

- a builder **persists and can return a build's log** (`QueryBuildLog`), and
- `BuildResult` carries **structured failure detail** (`failurePhase`,
  `exitCode`, `logTail`) and a **`logRef`** to fetch the full log later.

This lets `hydra-queue-runner` **retire its out-of-band log capture** and read
failure detail as fields instead of scraping strings — but only if/when Hydra
chooses to. **Nothing breaks for Hydra on day one**: the bump is gated behind a
`min(client, server)` handshake, so old Hydra ↔ new Nix and new Hydra ↔ old Nix
both keep working byte-for-byte at version ≤ 2.8.

**Two things we need from a Hydra maintainer** (the only blockers to freezing
serve diagnostic core):

1. **Sign off on the exact field set and byte order** below (or tell us what to
   change). We will name you as the sign-off owner in the decision record.
2. **A `hydra-queue-runner` branch** that (a) calls `QueryBuildLog` + consumes
   the structured log frames to drop its out-of-band log capture, and (b) reads
   the extended `BuildResult`, validated against a new-Nix builder.

Until both land (plus golden tests + one release-cycle soak), we keep the fields
behind an **unstable** version and do **not** bump `SERVE_PROTOCOL_VERSION`.

---

## Why this is worth it for Hydra specifically

`hydra-queue-runner` is the largest consumer of the serve protocol, via the
deliberately stripped-down `BasicClientConnection`/`BasicServerConnection`
(`serve-protocol.hh:96-101`), and `BuildDerivation` is annotated "Used by
hydra-queue-runner" (`nix-store.cc:1020`). Today the serve build path:

- **suppresses and discards the build log** on the builder
  (`nix-store.cc:908-909`: `verbosity = lvlError; keepLog = false`), so the log
  has to be captured out-of-band; and
- returns failures as a **free-text message** (`BuildResult::Failure::message()`),
  so phase/exit/tail must be parsed back out of a string.

The proposal removes both pain points at the source: the builder persists the
log and returns it on request, and failure detail arrives as typed fields. Hydra
gets to delete code, not add it.

---

## The proposed serve diagnostic core surface (frozen candidate)

This is the **stable diagnostic core** from decisions
[Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51).
The **absolute minimum** that unblocks the log-fetch win is `logRef` +
`QueryBuildLog`; `failurePhase`/`exitCode`/`logTail` are the recommended,
low-risk companions (they also deliver "fail loud" for the non-Hydra `ssh://`
build-remote hook).

### 1. Extended `BuildResult` fields

Appended **after** the existing 2.8 `builtOutputs` block, under a `>= {2,9}`
guard (see the version-number note below — the wire version is 2.9, not 3.0),
in **binary length-prefixed** form (the serve protocol is binary; JSON
appears only as the pre-existing 2.6 realisation back-compat shim, so we are
*not* using it here). No existing field changes meaning; a 2.8 reader stops
before the new tail.

| Field | Type | Meaning |
|---|---|---|
| `logRef` | string (store path) | the **resolved drv path** under which the builder asserts the log is persisted — the key `LogStore::getBuildLog` already uses; empty if not persisted |
| `failurePhase` | string (optional) | the build phase that failed (e.g. `configure`, `build`, `check`) |
| `exitCode` | int | builder exit status |
| `logTail` | list&lt;string&gt; | the last N log lines (the data `fixupBuilderFailureErrorMessage` embeds locally today, `derivation-building-goal.cc:1157`) |

**Deferred — NOT in the diagnostic core** (kept behind the unstable version because their
semantics depend on the still-spiking Phase 3 dedup/coordinator design):
`builderId`, `deduplicated`.

### 2. New operation: `QueryBuildLog`

- **Command enum value `= 10`** — the next free number after `AddToStoreNar = 9`;
  never sent unless the negotiated version supports it.
- **Request:** a derivation path (the `logRef` / resolved drv path).
- **Response:** the persisted log bytes (the same bytes `nix log` shows locally),
  or an empty/"absent" marker.
- Server-side this just exposes the existing `LogStore::getBuildLog`
  (`log-store.cc`, `local-fs-store.cc:160`) over the wire.

### 3. Builder-side change that makes the above meaningful

For versions that support log streaming, the serve server stops discarding the
log: make the `keepLog = false` / `verbosity = lvlError` suppression at
`nix-store.cc:908-909` conditional, and persist via `addBuildLog`. (This is RFC
Phase 0 and is independent of the dedup work.)

---

## Back-compat guarantee (no flag day, both directions)

Gated by the existing `min(client, server)` handshake
(`serve-protocol-connection.cc:8-33`) — **and shipped as a minor bump within
major 2** (`SERVE_PROTOCOL_VERSION = {2,9}`, *not* `{3,0}`; see the note below):

| Client | Server | Negotiated | Behaviour |
|---|---|---|---|
| old Hydra (≤2.8) | new Nix builder ({2,9}) | 2.8 | today's exchange; no new fields/op |
| new Hydra ({2,9}) | old Nix builder (≤2.8) | 2.8 | Hydra falls back to out-of-band log capture; `QueryBuildLog` not sent |
| new | new | {2,9} | full diagnostic core active |

The CA-realisation fields Hydra already consumes
(`std::map<OutputName, UnkeyedRealisation>` at 2.8) are **untouched**.

> **Why minor-within-major-2 and not "3.0" on the wire.** The serve **client**
> handshake hard-rejects a server with a different major
> (`serve-protocol-connection.cc:18`: `if (remoteVersion.major != 2 || … ) throw`),
> and that check runs **before** `min()`. Since the builder is the *server* and
> Hydra is the *client*, a `{3,0}` builder would make **every already-deployed
> Hydra/`nix` client throw at handshake** — the common "upgrade the build farm
> first" case breaks, and the guard in shipped clients can't be patched
> retroactively. So the diagnostic core ships as serve **2.9** (gated `>= {2,9}`),
> exactly like every prior serve feature; old clients negotiate `min` down to 2.8
> and keep working. The *wire* version is
> **2.9** (major stays 2). (Avoids the `(3 << 8 | 0)` major bump — see decisions Blocker 3,
> "Compatibility correction".)

---

## What "frozen" requires (all four; we will not bump the wire version until they hold)

1. **A named Hydra maintainer** has reviewed and signed off on the exact frozen
   field set and byte order. *(This thread exists to get that name.)*
2. A **`hydra-queue-runner` branch** consumes `QueryBuildLog` + the structured
   log frames to drop its out-of-band log capture **and** reads the extended
   `BuildResult`, validated against a new-Nix builder.
3. **Golden/characterisation tests** prove round-trip at 2.8 and 2.9 **and** that
   a 2.8 peer ignores 2.9 fields (full back-compat matrix, both directions).
4. The field set has soaked on the **unstable version for ≥1 release cycle** with
   no layout change.

Meanwhile (1)/(3) Nix-side work — the unstable-version implementation and the
golden tests — proceeds now so that by the time Hydra is ready, only sign-off and
the queue-runner branch remain.

---

## Specific questions for Hydra maintainers

1. **Field set:** is `{ logRef, failurePhase, exitCode, logTail }` the right core,
   or does the queue-runner need anything else as a *frozen* field (vs. something
   that can stay unstable)? In particular — is `logTail` useful to Hydra, or does
   `logRef` + `QueryBuildLog` (fetch the whole log) suffice for your UI?
2. **`logRef` semantics:** is "resolved drv path the builder asserts it persisted
   under" the right contract, or do you want an explicit "log present" boolean
   distinct from an empty path?
3. **`QueryBuildLog` shape:** is a single request→bytes op enough, or do you want
   ranged/streamed fetch for very large logs?
4. **Migration appetite:** would you prefer the queue-runner branch to (a) adopt
   `QueryBuildLog` only first (smallest change), then (b) the structured
   `BuildResult` later — or both at once?
5. **Who is the sign-off owner** for the frozen serve diagnostic core layout on the Hydra
   side?
6. **Version number:** we propose shipping the core as serve **2.9** (minor bump,
   major stays 2) rather than `{3,0}`, because the client handshake rejects a
   different major before `min()` and a `{3,0}` builder would break every already
   deployed client. Does the queue-runner rely on the major number anywhere (e.g.
   feature gating on `GET_PROTOCOL_MAJOR`) such that a minor-only bump is a
   problem on your side?

## Links

- RFC: [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md) (esp. §4.4, §4.5, §4.8, §7)
- Decision: [Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51)
- Execution/tests: [validation plan, Workstream D](./remote-build-protocol-redesign.validation.md)
