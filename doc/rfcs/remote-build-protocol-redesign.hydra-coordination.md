# Draft post: serve protocol diagnostic core — Hydra coordination

| | |
|------------------|------------------------------------------------|
| **Status**       | Draft for posting (Hydra ↔ Nix coordination thread) |
| **Audience**     | Hydra maintainers (esp. `hydra-queue-runner`), Nix serve-protocol maintainers |
| **Realizes**     | validation plan [Workstream D / D4](./remote-build-protocol-redesign.validation.md); decisions [Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51) |
| **Goal of thread** | **solicit** Hydra review of the serve diagnostic-core field set + byte order *during the soak* (input we want, not a freeze blocker), and flag the *later* round — agreeing the deferred serve set (`deduplicated`/`builderId`) once Phase 3 settles (gated on that design maturing, not on Hydra) |

> This is the **opening post** for the coordination thread the RFC deliberately
> left out of its own scope (RFC §10 Q4: "the Hydra coordination *thread* itself
> remains out of scope for this document"). Everything technical is already
> decided on the Nix side. **Note (revised 2026-06):** the diagnostic core
> freezes on Nix-side criteria + a soak — a field-by-field audit showed it
> carries nothing Hydra-specific (every field is justified by the non-Hydra
> `ssh://` hook / `nix log`), so your sign-off is **valuable input we're
> soliciting, not a blocker**. We'd genuinely like your review while the layout
> is still unstable and cheap to change. Copy/adapt this into the actual
> discussion (GitHub Discussions / Discourse / matrix) when posting.

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

**Two things we'd value from a Hydra maintainer** (review we're soliciting — *not*
freeze blockers, revised 2026-06):

1. **Eyes on the exact field set and byte order** below — tell us if anything is
   wrong *while it's still unstable and cheap to change*. We'll record your review
   in the decision record. (We freeze on Nix-side sign-off + an in-tree consumer +
   golden tests + a soak; your review rides the soak.)
2. **Whenever it suits you, a `hydra-queue-runner` branch** that calls
   `QueryBuildLog` + consumes the structured log frames to drop its out-of-band
   log capture, and reads the extended `BuildResult`. This is **opt-in adoption on
   your schedule** (G7 "Hydra is never forced"), not a precondition of our freeze.

We keep the fields behind an **unstable** version until the Nix-side criteria +
soak hold; old Hydra keeps working byte-for-byte at ≤2.8 the entire time
regardless. The round where we'll want to converge on a layout is later: the
**deferred set** (`deduplicated`/`builderId`) once Phase 3 dedup semantics
settle. Those fields are deferred because their *semantics* aren't frozen yet
(not because they're Hydra-specific — if anything they're more
elastic-backend/introspection-shaped; see the deferred-fields note below), so
that round is gated on the Phase 3 design, with your review again solicited
rather than required.

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
semantics depend on the still-spiking Phase 3 dedup/coordinator and
elastic-backend designs): `builderId`, `deduplicated`, and the elastic-backend
failure-classification fields (a transient/retryable flag, failure-class, and a
resource hint; RFC §4.4). These are listed only so Hydra sees the shape that is
coming; **none is part of the frozen ask** in *this* round. They become a
**later field-set round on this same serve channel** — gate **H3** in the
validation plan, which freezes the Phase-3 "Build Session" serve surface
(`F-WIRE`) — gated on the Phase-3 dedup/elastic-backend design settling (a
*design-maturity* gate, internal — these fields aren't Hydra-specific; Hydra
picks its own builder and runs its own queue, so they matter more to a client
that didn't choose the builder, e.g. orchestrator backends / `QueryActiveBuilds`).
So this thread carries two rounds on one wire: the diagnostic core now (review),
the deferred set later — your review solicited in both, blocking in neither. (The
worker-protocol versions of these ops are
not part of any Hydra ask — `hydra-queue-runner` speaks only the serve protocol.
Full map: [validation plan, "The external gates (Hydra)"](./remote-build-protocol-redesign.validation.md#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).)

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

## What "frozen" requires (revised 2026-06 — all Nix-side; we will not bump the wire version until they hold)

1. **The libstore/serve-protocol maintainer** has signed off on the exact frozen
   field set and byte order.
2. **At least one in-tree serve consumer** exercises the core end-to-end: `nix
   log` over the serve path (`QueryBuildLog`) **and** the `ssh://` `build-remote`
   hook rendering `logTail`/`failurePhase`/`exitCode` on failure. *(This is what
   proves the layout against a real consumer — Hydra's branch is welcome but not
   required for it.)*
3. **Golden/characterisation tests** prove round-trip at 2.8 and 2.9 **and** that
   a 2.8 peer ignores 2.9 fields (full back-compat matrix, both directions).
4. The field set has soaked on the **unstable version for ≥1 release cycle** with
   no layout change — *the window in which we want your review (above)*.

**Why none of these is a Hydra blocker:** compatibility is unconditional via
`min()`, and a field-by-field audit shows the frozen core is non-Hydra-specific
(every field is justified by the non-Hydra `ssh://` hook / `nix log`). The
remaining fields are all in the deferred set, whose freeze is a later round —
gated on the Phase 3 design settling (design-maturity, internal), not on Hydra
(decisions [Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51)).

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
5. **Who's our best reviewer** for the diagnostic-core layout on the Hydra side —
   and, looking ahead, the right person to agree the *deferred* set
   (`deduplicated`/`builderId`) with us once Phase 3 settles (that later round is
   where we genuinely need to converge together)?
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
