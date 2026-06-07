# Decisions: the three blockers before the Phase 3 / serve diagnostic-core wire freeze

| | |
|------------------|------------------------------------------------|
| **Status**       | Decision record (converged) — unblocks Phase 3 wire design |
| **Parent**       | [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md) |
| **Spike**        | [`remote-build-protocol-redesign.spike.md`](./remote-build-protocol-redesign.spike.md) |
| **Validation**   | [`*.validation.md`](./remote-build-protocol-redesign.validation.md) — the executable plan for these decisions' owed prototypes/tests (the "Follow-up" sections below are indexed there) |
| **Resolves**     | RFC Q2 (cancel matrix), Q3 (CA resolution timing), Q4 (Hydra field set), Q7 (reuse-key vs. trust); spike §5.4.1–.2, §6 Q2/Q3 |
| **Working group**| libstore/protocol maintainers, a Hydra maintainer, a security reviewer |

> This document **converges on decisions** — not options — for the three issues
> that gate freezing any new wire surface for remote-build dedup/attach (RFC
> Phase 3) and the extended `BuildResult` (serve diagnostic core). Everything here is bounded
> by the three invariants the RFC and spike establish and that must not break:
> (1) the **trust model** (RFC §6) — dedup must never become a cross-tenant
> log/result/existence oracle; (2) **no Hydra/Nix flag day** — every change is
> additive and version-gated via the `min(client,server)` handshake
> (`serve-protocol-connection.cc:8-33`); (3) **below-the-wire** — the
> client-facing Build Session surface is identical whether the backend is a
> single-process/elastic service, a distributed/multi-node orchestrator
> (RFC §4.3.4), or the stock fork-per-connection daemon + coordinator
> (spike §5.1, §5.3).
>
> The coordinator interface (spike §3) is taken as settled: the registry,
> replay buffer, and refcounts live in one coordinator process, behind a
> peer-credential-verified control socket (§3.7.1), with authorization
> re-derived before subscribe (§3.7.2). These decisions build on that and do
> not reopen it.

---

## Blocker 1 — Trust under CA key-merge (RFC Q3/Q7, spike §3.8/§5.4)

### 1. Decision

The dedup/reuse key is **the store path of the *resolved* derivation** — the
derivation with every `inputDrv` replaced by the concrete store path of that
input's realised output (the same resolution `DerivationResolutionGoal`
computes). Because resolution substitutes concrete, content-addressed input
paths, this key *is* nixbuild.net's "derivation + content of its inputs"; we do
**not** key on output/realisation hashes (those are the *result*, not the build
unit, and do not exist yet when two requests race). **Resolution is a
client-side precondition of `START_OR_ATTACH`:** the client resolves and sends
the *resolved derivation itself*; the coordinator does **not** resolve (it has
no eval-store inputs) but **derives the key by hashing the resolved derivation
it received** — it never trusts a client-asserted key string. Authorization is
**per-observable and re-derived per-subscriber against the resolved key**,
using the existing `BuildDerivation`-handler trust check (the `daemon.cc` trust
comment referenced from `build-remote.cc:324-329`), **before** the registry is
consulted, so that *whether a build exists* is never revealed to an unauthorized
caller (no existence/timing oracle): a denied caller gets a uniform denial whose
content and timing do not branch on registry membership. The `resolving`
pre-state (spike §3.8) carries **no inherited authorization**; when provisional
entries promote/merge onto one resolved key, **every subscriber is
re-authorized against the resolved key at promotion**, and any subscriber that
fails is detached with an error. The merge cannot leak one caller's inputs to
another because the shared `Build` contains *only* the resolved drv (inputs
already collapsed to concrete content-addressed paths), its log, and its
outputs — all of which any co-authorized subscriber could obtain itself; the
other caller's *unresolved* drv and private eval-store sources are never
represented in the shared `Build`. Dedup therefore introduces **no new
input-reuse channel**: whether a client may use an input path remains the
existing signature/CA/trusted-user check, applied at resolution time, before the
key exists.

### 2. Rejected alternatives

- **Key on the input-addressed `.drv` path.** Rejected: misses CA coalescing
  (two distinct `.drv`s that resolve identically would not dedup), contradicting
  the RFC's resolved-key intent (§4.3) and nixbuild.net's reuse semantics (§4.7).
- **Key on the CA realisation / output hash.** Rejected: the output does not
  exist while a build is in flight, so it cannot be the *attach* key; it is the
  result, not the work unit.
- **Coordinator performs resolution.** Rejected: resolution needs eval-store-only
  `.drv` inputs the coordinator may not have (spike §2.2, §5.4.1); giving the
  coordinator an eval store widens its trust surface and creates a second place
  where input trust must be enforced. Client-side resolution keeps the
  coordinator a pure key-and-authorize component.
- **Trust the client's asserted key string.** Rejected: a buggy/malicious child
  could present a `buildKey` that does not match `drvForBuild` and attach to a
  different tenant's build. The coordinator instead recomputes the key from the
  resolved drv it receives (cheap deterministic hashing, no eval store needed).
- **Authorization = union or intersection of the merged unresolved-drv
  authorizations.** Rejected. *Union* lets a caller authorized only for
  unresolved-B observe a build it could not have requested → cross-tenant leak.
  *Intersection* denies legitimately co-authorized callers and still inherits a
  stale per-unresolved-drv decision. The only correct basis is the **resolved
  key itself**, re-checked per subscriber.
- **Reveal MISS/HIT (or even just answer faster on HIT) before authorization.**
  Rejected: that is precisely the existence/timing oracle RFC §6 forbids.

### 3. What it commits

- **Wire/interface:** `START_OR_ATTACH` carries the **resolved derivation** plus
  the authenticated `sessionAuth` (spike §3.2); the **build key is defined as
  the hash/store-path of that resolved derivation**, computed by the
  coordinator. This is the load-bearing semantic commitment — the identity of
  "the same build" is frozen here.
- **Semantics:** authorization is **per-observable** and checked **before**
  registry lookup; the set of observables and their gate is frozen as:

  | Observable | Who may observe |
  |---|---|
  | Existence (MISS vs. HIT) / timing | Only after authz; denial is uniform and does not branch on existence |
  | Live log + replay buffer | Authorized-to-build-the-resolved-key subscribers only |
  | `BuildResult` (status, `builtOutputs`, output paths) | Same |
  | `deduplicated` flag | Same (allowed: a co-authorized party learning the *same CA build* is in flight is not a cross-tenant secret) |
  | `QueryActiveBuilds` entry | Same authz filter; else aggregate count or nothing (Q5) |

- **Semantics:** the `resolving` pre-state exists but is **authorization-free**;
  authorization materializes only at promotion against the resolved key. The
  shared `Build` is committed to contain *only* resolved-drv-derived state.

### 4. Residual risk and the guarding test

- **Risk:** a `deduplicated=true` result is a (weak) signal that *someone else*
  is/was building the same resolved drv. Bounded: only co-authorized parties for
  that exact CA build can ever see it, and untrusted clients can only dedup on CA
  derivations (input-addressed builds by untrusted clients are rejected by the
  unchanged trust check), so the signal never crosses an authorization boundary.
- **Risk:** key-derivation correctness — if the coordinator's key hashing
  diverges from the resolution the client performed, two genuinely-identical
  builds fail to coalesce (a correctness/efficiency miss, not a leak).
- **Guarding tests (RFC §9 "Trust tests"):**
  1. **Cross-tenant attach denied (existence-oracle):** an unauthorized client
     issuing `START_OR_ATTACH`/`QUERY_ACTIVE` for a resolved key that *is*
     in-flight, and for one that is *not*, receives byte-identical denials with
     no measurable timing difference — it learns neither the log nor the drv
     name nor whether the build exists.
  2. **CA merge re-auth:** two distinct unresolved drvs (caller A authorized,
     caller B not for the *resolved* key) that resolve to the same key — assert B
     is detached with an error at promotion and observes none of A's log/inputs;
     assert the shared `Build` exposes only resolved-drv state.
  3. **Asserted-key spoof:** a child presenting a `buildKey` that does not match
     `drvForBuild` is rejected (coordinator recomputes from the resolved drv).

### 5. Owner + follow-up

- **Owner:** security reviewer + libstore/protocol maintainer (joint —
  this is the trust linchpin).
- **Follow-up spike:** extend the spike's input-addressed prototype (§4) with
  the **CA `resolving`→promote** path and the **re-authorize-on-promotion**
  check; this is explicitly out of scope for the first prototype (spike §4.3) and
  is the one remaining unproven mechanism. Land trust tests (1)–(3) before any
  Phase 3 wire freeze.

---

## Blocker 2 — The refcounted-cancel matrix (RFC Q2, spike §5.2)

### 1. Decision

A shared `Build` lives **purely by refcount**: it continues while ≥1 subscriber
is attached and is cancelled when the refcount hits zero **unless an explicit
build/GC root** (a durable "I want this output") is registered — that, and *only*
that, is `hasRootReasonToContinue()`. There is **no originator privilege**: the
first subscriber is just subscriber #1, so "originator detaches while a joiner
remains" simply means refcount ≥ 1 and the build continues. **`--keep-going`
does not extend a shared build's lifetime** (refining the RFC's wording, which
lumped it with roots): keep-going governs whether a *client's sibling*
derivations proceed after one fails, which is per-client scheduling, not this
build's lifetime. **Timeouts are per-subscriber deadlines that only detach that
subscriber**: the shared build runs under the **maximum (most-generous)
envelope** of currently-attached subscribers' `maxSilentTime`/`buildTimeout`,
recomputed as subscribers leave; a subscriber whose own (shorter) deadline
elapses receives a `TimedOut` `BuildResult` and detaches (decrementing the
refcount) **without** cancelling the build for others — strictest-wins is
rejected because it is a cross-tenant DoS. **`--keep-failed` is a logical OR**:
because the failed build directory is a single artifact on the builder, if any
currently-attached subscriber requested it, the coordinator preserves it (under
the builder's existing on-disk trust/policy). **An active cancel is an
unsubscribe-with-error scoped to the cancelling client only**: that client
synthesizes its own local interrupt and sends `CANCEL_HINT`/`UNSUBSCRIBE`; no
new wire `BuildResult` status is needed, the build is unaffected if others
remain, and only still-attached subscribers receive the build's real
`BuildResult`.

### 2. Rejected alternatives

- **Strictest-timeout-wins for the shared build.** Rejected: a subscriber
  attaching with a 1-second timeout would kill every co-subscriber's build — a
  trivial cross-tenant denial-of-service.
- **Originator's timeout / originator owns the build.** Rejected: privileges
  subscriber #1, lets it cancel joiners' work, and couples otherwise-independent
  tenants. Refcount with no originator is the only symmetric rule.
- **`--keep-going` keeps an unwatched build alive.** Rejected as a category
  error: keep-going is about sibling targets in *one client's* request, not about
  finishing a build nobody is watching. Folding it into the root-reason set makes
  lifetime depend on an unrelated flag.
- **`--keep-failed` honored per-subscriber.** Impossible: the failed dir is one
  shared artifact on the builder; you cannot keep it for one viewer and delete it
  for another. OR is the only coherent rule.
- **Cancel = cancel the shared build.** Rejected: that is exactly today's 1:1
  `MonitorFdHup` semantics (spike §1.5) that the refcount design exists to
  invert; it would let any one client tear down everyone's build.
- **New `Cancelled` `BuildResult` failure status on the wire.** Rejected as
  unnecessary surface: the cancelling client already knows it cancelled and
  synthesizes the interrupt locally; the wire needs only `CANCEL_HINT`/
  `UNSUBSCRIBE`.

### 3. What it commits

The complete state→action table (directly translatable to tests, mirroring RFC §9):

| Event | Condition | Shared build | Result to acting client | Results to others |
|---|---|---|---|---|
| Subscriber detaches (HUP) | refcount → >0 | continue | none (gone) | unaffected |
| Subscriber detaches | refcount → 0, no root | **cancel**; release output `PathLocks`; drop registry entry | none | n/a |
| Subscriber detaches | refcount → 0, **explicit root present** | **continue to completion**, then exit; persist log | none | result persisted/fetchable |
| Late joiner subscribes in the "about to cancel" window | refcount 0 → 1 (race-free under the single-threaded registry) | **survive** (re-increment), continue | replay + live | — |
| Subscriber active-cancel (`CANCEL_HINT`) | refcount → >0 | continue | local interrupt (client-synthesized) | unaffected |
| Subscriber active-cancel | refcount → 0, no root | cancel | local interrupt | n/a |
| Subscriber's own timeout elapses | other subscribers remain | continue under `max` of remaining deadlines | `TimedOut` `BuildResult`, detach | unaffected |
| All subscribers' deadlines elapsed | ≡ refcount 0 by timeout | cancel (timeout) | `TimedOut` | `TimedOut` to each |
| Build fails | `--keep-failed` set by ≥1 attached subscriber | preserve failed dir (**OR**) | `Failure` (+ phase/exit/tail) | `Failure` to all |
| Build fails | no `--keep-failed` | clean up | `Failure` | `Failure` to all |
| Build completes | — | deliver `BuildResult` to all attached; persist log | `Success` | `Success` to all |

Frozen rules: **lifetime = refcount + explicit-root only**; **no originator
privilege**; **timeout = per-subscriber detach under a max envelope**;
**keep-failed = OR**; **cancel = per-subscriber unsubscribe-with-error, no new
wire status**.

### 4. Residual risk and the guarding test

- **Risk:** `--keep-failed` OR lets one subscriber cause the builder to retain a
  failed dir that a co-subscriber did not ask for (disk use). Bounded: it is the
  builder operator's disk, already governed by the builder's own keep-failed
  policy, and already reachable by a single untrusted client today.
- **Risk:** the max-envelope timeout means a long-deadline subscriber keeps a
  build running that a short-deadline subscriber has already abandoned — by
  design (the long-deadline subscriber still wants it), and bounded by that
  subscriber's own deadline and refcount.
- **Guarding test (RFC §9, extends the spike §4.2 cancel test):** a
  `build-dedup-cancel` functional matrix —
  (a) two attached, kill subscriber #1 (the originator) → build **continues**,
  #2 completes; (b) one attached, disconnect it, no root → build **cancels**
  (marker counter does not reach completion); (c) one attached **with an explicit
  root**, disconnect it → build **completes**; (d) two attached with timeouts
  10 min / 60 min → at 10 min the first gets `TimedOut` and detaches, build
  continues for the second; (e) build fails with keep-failed set by exactly one
  of two subscribers → failed dir is preserved; (f) active-cancel by one of two →
  canceller gets a local interrupt, the other still completes.

### 5. Owner + follow-up

- **Owner:** libstore/protocol maintainer (build scheduling / `Worker`
  lifetime).
- **Follow-up spike:** implement `hasRootReasonToContinue()` as
  *explicit-root-only* and the **per-subscriber-deadline + max-envelope** timer
  in the coordinator (the spike §5.2 stubs this to "false"); encode the table
  above as the `build-dedup-cancel` test. No public-wire change is required —
  this is entirely coordinator-internal, so it does **not** gate the wire freeze,
  but the table must be agreed before Phase 3 implementation.

---

## Blocker 3 — The Hydra field set & serve diagnostic-core freeze (RFC Q4, §7, spike §5.1)

### 1. Decision

Split the extended `BuildResult` into a **stable diagnostic core** and a
**deferred dedup/fleet-observability set**, and freeze only the former for serve
2.9. The **stable core** (the smallest set that lets `hydra-queue-runner` retire
its out-of-band log handling and report failures structurally) is: `logRef` (the
resolved drv path the builder asserts it persisted the log under, the key
`LogStore::getBuildLog` already uses), structured failure detail
`failurePhase` + `exitCode` + `logTail`, **plus a new `QueryBuildLog` serve
operation**. The **deferred set** — `builderId` and `deduplicated` — stays behind
an **unstable/experimental serve version** because its semantics are defined by
the Phase 3 coordinator/dedup design, which is explicitly *not* frozen; freezing
it now would overcommit to dedup semantics still in spike. **The elastic-backend
failure-classification fields (the transient/retryable flag, failure-class, and
resource hint of RFC §4.4) join this deferred set** for the same reason — their
semantics depend on the still-spiking elastic-backend design (RFC §8.1
guardrail 7). The absolute
**minimum to unblock Phase 1 without overcommitting is `logRef` +
`QueryBuildLog`** (the two that let Hydra fetch the persisted log instead of
capturing it inline); `failurePhase`/`exitCode`/`logTail` are the recommended,
low-risk companions that also deliver fail-loud (G8) for non-Hydra serve clients.
Serialization **extends the existing conditional, version-gated serve
serializer ladder** (`serve-protocol.cc:29-96`, the `>= {2,3}/{2,6}/{2,8}`
pattern): new fields are appended **after** the 2.8 `builtOutputs` block under a
`>= {2,9}` guard, in **binary length-prefixed** form (not JSON — JSON is used
only for the pre-existing 2.6 realisation back-compat hack), so a 2.8 reader
stops before them and **no existing field changes meaning**; the CA realisation
fields Hydra already consumes (`std::map<OutputName, UnkeyedRealisation>` at 2.8)
are untouched. `QueryBuildLog` is a **new `Command` enum value (`= 10`, the next
free number after `AddToStoreNar = 9`)**, never sent unless the negotiated
version supports it. **`SERVE_PROTOCOL_VERSION` is NOT bumped to `(2 << 8 | 9)`
until the freeze criteria below are met**; until then the fields live behind an
explicitly-unstable provisional gate whose byte layout is *not* a back-compat
promise.

**Freeze criteria for serve diagnostic core (all must hold):**
1. A **named Hydra maintainer** has reviewed and signed off on the exact frozen
   field set and byte order.
2. A `hydra-queue-runner` branch (a) consumes `QueryBuildLog` + the structured
   log frames (§4.2) to **drop its out-of-band log capture**, and (b) reads the
   extended `BuildResult`, both validated against a new-Nix builder.
3. **Golden/characterisation serialization tests** (`src/libstore-tests`,
   `src/json-schema-checks`) prove round-trip at 2.8 and 2.9 **and** that a 2.8
   peer ignores 2.9 fields — the full back-compat matrix, both directions.
4. The field set has been carried on the **unstable version for ≥1 release
   cycle** with no layout change.

Only then bump `SERVE_PROTOCOL_VERSION` to `(2 << 8 | 9)` and treat the layout
as frozen.

### 2. Rejected alternatives

- **Freeze the full field set (incl. `builderId`/`deduplicated`) as 2.9 now.**
  Rejected: their meaning depends on the unfrozen Phase 3 coordinator/dedup
  design; freezing a byte layout for semantics still in spike risks a later
  Hydra/Phase-3-driven change breaking the very back-compat §7 promises — exactly
  the retroactive-break the RFC warns against (§7 final bullet).
- **JSON-encode the new fields.** Rejected: the serve protocol is binary
  length-prefixed; JSON is present only as a 2.6 realisation compat shim
  (`serve-protocol.cc:81-95`). New scalar/string fields (`exitCode`,
  `failurePhase`, `logRef`, `logTail`) serialize natively and cheaper as binary.
- **Bump `SERVE_PROTOCOL_VERSION` to 2.9 immediately to start using the
  fields.** Rejected: a shipped 2.9 layout is a permanent back-compat promise
  (§7). Prototype behind an unstable gate first; bump only at freeze.
- **Bump the serve *major* version (to `(3 << 8 | 0)` / "3.0").** **Rejected —
  this is the decision recorded here.** The serve *client* handshake hard-rejects
  a server whose major differs, **before** `min()`
  (`serve-protocol-connection.cc:18`), and that guard is baked immutably into
  every already-shipped client. Since the builder is the server and Hydra/`nix`
  is the client, a major-3 builder breaks every deployed client (old client →
  upgraded builder) — a real regression `min()` cannot save, and no feature flag
  can fix retroactively. **Decision: the diagnostic core ships as a *minor* bump
  within major 2 (`(2 << 8 | 9)`), exactly like every prior serve feature.** A
  genuine major bump is deferred to a future breaking change, and only after a
  guard-relaxing client release has propagated for ≥1 cycle. See §3
  ("Compatibility correction") for the full reasoning and the guarding test.
- **Make Hydra adopt the new log channel as a precondition of Phase 1.**
  Rejected: violates "Hydra is never forced" (G7). The path is additive and
  opt-in; Hydra adopts when it chooses, and `min()` fallback keeps old Hydra
  working.
- **Drop `logTail` from the core (Hydra has the full log via `logRef`).**
  Considered but kept: `logTail` is low-risk and is what delivers fail-loud (G8)
  for the *non-Hydra* serve client (the `ssh://` build-remote hook), so it earns
  its place in the frozen diagnostic core even though Hydra itself does not need
  it.

### 3. What it commits

- **Wire layout (frozen at 2.9):** the append-after-2.8-`builtOutputs`,
  `>= {2,9}`-gated, binary order of `logRef`, `failurePhase`, `exitCode`,
  `logTail` in the `BuildResult` serializer; and `QueryBuildLog` as
  `Command = 10`. Once 2.9 ships this is a back-compat promise.
- **Semantics:** `logRef` = the resolved drv path under which the builder
  asserts the log is persisted (`LogStore::getBuildLog` key); structured failure
  = phase/exit/tail as *fields*, not baked into the message string.
- **Deferred (explicitly NOT frozen):** `builderId`, `deduplicated` — carried on
  the unstable version, free to change until Phase 3 semantics settle. The RFC
  §4.4 failure-classification fields (transient/retryable flag, failure-class,
  resource hint) are deferred on the same basis (RFC §8.1 guardrail 7).
- **Back-compat matrix (both directions, via `min()` handshake):**

  | Client | Server | Negotiated | Behaviour |
  |---|---|---|---|
  | new Hydra (client, ≥2.9) | old Nix builder (server, ≤2.8) | 2.8 | Hydra falls back to out-of-band log capture; `QueryBuildLog` not sent ✅ |
  | new | new | 2.9 | full diagnostic core active ✅ |

> **Compatibility correction (2026-06) — the version number must stay in major
> 2; do *not* bump to `(3 << 8 | 0)`.** The `min()` handshake only protects
> back-compat *within the same major*. The serve **client** handshake hard-rejects
> a server whose major differs (`serve-protocol-connection.cc:18`:
> `if (remoteVersion.major != 2 || remoteVersion < {2,5}) throw "unsupported …"`),
> and this check runs **before** `min()`. Because the builder runs
> `nix-store --serve` (server) and Hydra / `nix build --store ssh://` is the
> client, the breaking direction is **old client → upgraded (major-3) builder**:
> the old client reads the builder's `{3,0}`, sees `3 != 2`, and throws — the
> connection never reaches field negotiation. This is the common
> "upgrade the build farm before all schedulers" case, so it is a real
> regression, not a corner case. Already-shipped clients carry this guard
> immutably, so a later relaxation cannot fix them retroactively.
>
> **Resolution:** ship the diagnostic core as a **minor bump within major 2**
> (next free minor — `SERVE_PROTOCOL_VERSION = (2 << 8 | 9)`), gated `>= {2,9}`,
> exactly as every prior serve feature has been. Then old client → new builder
> negotiates `min({2,8},{2,9}) = {2,8}` and works unchanged. The wire
> version is **2.9** (major stays 2). A genuine
> major bump is deferred to a future breaking change, and only after a
> guard-relaxing client release has propagated for ≥1 cycle. The corrected
> first matrix row:
>
> | old client (≤2.8) | new builder | negotiated | behaviour |
> |---|---|---|---|
> | **at `{2,9}`** (resolution) | reads `{2,9}` → `2==2` ✓ | **2.8** | today's exchange; no new fields/op ✅ |
> | ~~at `{3,0}`~~ (rejected) | reads `{3,0}` → `3≠2` | — | **handshake throws; build fails** ❌ |

### 4. Residual risk and the guarding test

- **Risk:** freezing the core before Hydra's branch is proven could still miss a field
  Hydra needs. Mitigated by freeze criterion 2 (a working queue-runner branch is
  a *precondition* of the freeze) and criterion 4 (a soak cycle on the unstable
  version).
- **Risk:** `logRef` asserts persistence but a builder might not actually have
  retained the log (e.g. `keepLog` still suppressed on an older path). Mitigated
  by pairing `logRef` with the explicit "persisted" assertion (RFC §4.4) and by
  Phase 0 making `keepLog`/`verbosity` suppression conditional
  (`nix-store.cc:908-909`).
- **Risk (the §3 compatibility correction):** a **major** version bump (`{3,0}`)
  is rejected by every already-deployed client at handshake
  (`serve-protocol-connection.cc:18`, `major != 2`), breaking old client → new
  builder. Mitigated by shipping the core as a **minor** bump within major 2
  (`{2,9}`), so it negotiates down transparently like every prior serve feature;
  a real major bump is deferred behind a propagated guard-relaxing release.
- **Guarding test (RFC §9 "Protocol characterisation tests"):** golden
  serializations of `BuildResult` at 2.3/2.6/2.8/2.9 and of a `QueryBuildLog`
  round-trip; an explicit **2.8-reads-2.9-bytes** test asserting the 2.8 reader
  consumes exactly the 2.8 fields and the negotiated-down peer never emits the
  2.9 tail; plus a functional test that `nix log` over the serve path returns the
  real log via `QueryBuildLog` (Gap A/§4.5); **and a handshake test that an
  old client (`major == 2` guard) reaching a `{2,9}` builder negotiates `{2,8}`
  and connects — the regression a `{3,0}` major bump would cause.**

### 5. Owner + follow-up

- **Owner:** **Hydra queue-runner maintainer** (sign-off authority on the frozen
  field set — to be named when the Hydra coordination thread opens; this record
  assigns the *role* and the gate), with a **libstore/serve-protocol maintainer**
  as the Nix-side counterpart who owns the serializer and the version bump.
- **Follow-up:** open the Hydra coordination thread (RFC Q4 says coordination is
  out of scope of the RFC itself); land the characterisation tests and the
  unstable-version implementation of the diagnostic core in Phase 1; **do not bump
  `SERVE_PROTOCOL_VERSION` to 2.9 until the four freeze criteria are met.**

---

## Operational decision O1 — Coordinator deployment model (spike §6 Q1)

> This is an **operational** decision, not a wire blocker: it concerns *how the
> coordinator process is deployed, owned, and reaped*, not the public protocol.
> It builds on the settled coordinator interface (spike §3) and does not reopen
> it. It changes **no public wire** (the coordinator is below the wire, spike
> §5.1), so it does **not** gate the Phase 3 / serve diagnostic-core freeze; it is recorded
> here so the coordinator can be implemented without a further design round.

### 1. Decision

The coordinator is **the existing daemon binary run in a new `--coordinator`
role — a separate process, not a separate codebase, and not in-process with the
listener.** Concretely:

- **Same binary, distinct role.** The coordinator hosts a real `Worker`
  (build + eval store), `fork()`s builders, and runs the *exact* trust check the
  `BuildDerivation` handler already enforces (RFC §6; the `daemon.cc` trust
  comment referenced from `build-remote.cc:324-329`). All of that already lives
  in the daemon binary, so the coordinator is a **role of that binary**
  (`nix-daemon --coordinator` / an internal re-exec), never a second codebase
  that would duplicate the libstore build + trust stack.
- **A separate process from the listener, supervised by the daemon parent.**
  The long-lived parent that runs `daemonLoop` (`src/nix/unix/daemon.cc:247`)
  is already the natural owner: it outlives every connection child, **already
  installs `SIGCHLD` handling and reaps children** (`serveUnixSocket` + the
  `waitpid` loop, `daemon.cc:299-321`), and is **already restarted by the
  service manager on crash** (`daemon.cc:290-296`, `activationName =
  "nix-daemon.socket"`, `:303`). **Canonical lifecycle: when a daemon parent
  starts against a store that supports the coordinator capability, it
  detects-an-existing-or-spawns *one* coordinator, becomes its parent, and reaps
  it**; the coordinator in turn reaps its own build subprocesses (spike §2.2
  cleanup row). This makes the spawn-election race (spike §6 Q8) vanish in the
  common case — only the single parent spawns — and keeps the listener a thin,
  always-available acceptor that can **respawn a crashed coordinator**, which is
  what makes the "degrade safely to `PathLocks`" claim (spike §2.2) real.
- **Lazy spawn is the *fallback only*** — for setups with no central daemon
  parent (direct multi-process store access, rootless/test harnesses). There the
  first child that needs the coordinator spawns it using the `bind()`/lock-file
  election and decline-and-respawn handshakes of spike §3.1, and the spawned
  coordinator `setsid()`s / double-forks to detach from its transient parent so
  it outlives the child that started it.
- **Socket.** One coordinator **per store directory**, at
  `$NIX_STATE_DIR/coordinator.socket` (e.g. `/nix/var/nix/coordinator.socket` in
  multi-user), mirroring how the daemon socket is already per-store. Created
  owner = daemon uid / `nix-daemon` group, mode **`0660`** (stricter than the
  daemon socket's own `0666` at `daemon.cc:302`, because only daemon children —
  never end-user clients — ever speak to it), peer-credential-verified per spike
  §3.7.1. A stale socket from a crashed coordinator is detected on connect and
  re-created on the next spawn.
- **Multi-user / systemd.** In the standard NixOS multi-user setup the daemon
  runs as root under systemd; the coordinator runs **as root too** (it must, to
  run the `Worker` / sandbox / chown outputs) and is **one per machine (one per
  store), not per-user** — dedup is deliberately *across* users on the same
  builder, which is exactly the cross-tenant case the trust model (spike §3.7,
  Blocker 1) is built for. Per-user isolation is enforced by the per-observable
  authorization check, **not** by a process boundary. Packagers **MAY**
  optionally split the coordinator into its own socket-activated unit
  (`nix-daemon-coordinator.{socket,service}`); the daemon's
  detect-existing-else-spawn logic makes that a drop-in optimization, not a
  requirement.
- **Single-user installs (no daemon) need no coordinator.** With direct local
  store access there is a single process with a single in-process `Worker`, so
  dedup is already the native `initGoalIfNeeded` path (spike §1.2). Coordinator
  presence is therefore gated on "a multi-user daemon exists."

### 2. Rejected alternatives

- **A separate coordinator codebase / standalone binary.** Rejected: it would
  have to link essentially all of libstore and **duplicate the trust check**,
  creating a second place where input/build authorization must be kept correct —
  the opposite of Blocker 1's "one place running the existing trust code."
- **Run the registry in-process in the listener parent (no separate
  coordinator process).** Rejected: it couples the *listening socket's*
  availability to build-execution load, and a `Worker`/scheduling crash would
  take down the **acceptor** too — so no new connections, and nothing left to
  respawn state. A separate, parent-supervised coordinator keeps the acceptor
  thin and lets it restart a crashed coordinator (the safe-degrade floor).
- **Pure lazy spawn as the canonical model.** Rejected as the default: it makes
  the spawn-election + idle-exit races (spike §6 Q8) load-bearing on every
  install and requires fragile reparenting. Kept strictly as the
  no-supervisor fallback.
- **Per-user coordinators.** Rejected: per-user processes cannot dedup *across*
  users — which defeats the feature's purpose on a shared builder — and trust is
  already handled per-observable (Blocker 1), so a process boundary buys nothing.

### 3. What it commits

- The `--coordinator` **role** of the daemon binary; the daemon parent's
  **detect-existing-else-spawn + supervise/reap** branch at `daemonLoop`
  startup; the lazy-spawn fallback path for non-daemon setups.
- Socket **path** (`$NIX_STATE_DIR/coordinator.socket`, per store), **mode**
  (`0660`), ownership, and peer-cred verification (spike §3.7.1).
- **One coordinator per store, as the daemon's uid (root in multi-user), not
  per-user.** No public-wire surface; nothing here is a back-compat promise.

### 4. Residual risk and the guarding test

- **Risk:** tying coordinator lifetime to the daemon parent means
  `systemctl restart nix-daemon` tears down in-flight shared builds. Bounded and
  **intentionally deferred to O2**: whether the coordinator must *survive* a
  daemon restart (persistent registry + re-adoption of running build
  subprocesses) is the **crash-recovery posture**, now decided in **O2**
  (safe-degrade, no persistence/re-adoption in v1) — the canonical "parent owns
  it" model degrades to the `PathLocks` floor on restart, which O1/O2 deem
  acceptable as the baseline.
- **Risk:** the optional socket-activated-unit packaging path and the
  parent-spawn path must not both spawn a coordinator. Resolved by the single
  detect-existing-else-spawn check (the same election as spike §3.1) being the
  *only* spawn entry point in both modes.
- **Guarding test:** a functional test that (a) two daemon parents / two
  children against one store end up with **exactly one** coordinator process and
  one socket; (b) a connection from a *different* uid to the coordinator socket
  is refused at the peer-cred check (spike §3.7.1, already in the §4.2 prototype
  list); (c) killing the coordinator leaves the listener up and a subsequent
  build **respawns** it and falls back to a correct (`PathLocks`-coalesced)
  build in the interim.

### 5. Owner + follow-up

- **Owner:** libstore/daemon maintainer (daemon lifecycle / `daemonLoop`), with
  a **packager/NixOS-module** reviewer for the systemd-unit-split option.
- **Follow-up:** fold the `--coordinator` role, the parent's spawn/supervise
  branch, and the `0660` peer-verified socket into the spike's throw-away
  prototype (spike §4.1, item 1) so deployment is exercised alongside
  start-or-attach; the daemon-restart-survival question is settled in **O2**
  (crash-recovery posture).

---

## Operational decision O2 — Coordinator crash-recovery posture (spike §6 Q5)

> Like O1, an **operational** decision with **no public-wire** surface, so it
> does not gate the Phase 3 / serve diagnostic-core freeze. It answers the question O1
> explicitly deferred ("must builds survive a coordinator/daemon restart?").

### 1. Decision

**v1 is safe-degrade: the coordinator keeps no cross-restart state, and a
coordinator (or daemon-parent) restart tears down in-flight *shared* builds —
correctly, never corruptly — falling back to today's `PathLocks` floor.** No
persistent registry and no re-adoption of running builders in the first
implementation. Concretely:

- **State is in-memory only.** The registry, replay buffers (already
  coordinator memory, spike §3.5), refcounts, and persisted-log writers do not
  survive a coordinator exit. A respawned coordinator (the `daemonLoop` parent
  respawns it per O1) starts empty; `QueryActiveBuilds` returns nothing until new
  builds register.
- **Builds share the coordinator's fate.** The coordinator `fork()`s builders
  with **`dieWithParent`** (`PR_SET_PDEATHSIG, SIGKILL`,
  `src/libutil/unix/processes.cc:248`) and/or in a coordinator-owned cgroup it
  can `cgroup.kill` (`src/libutil/linux/cgroup.cc:91`,
  `linux-derivation-builder.cc:324,786`). So a coordinator crash **kills its
  in-flight builds and releases their output `PathLocks`** — no orphan keeps a
  lock held or keeps writing into a now-dead log pipe (which would only earn an
  `EPIPE` and waste/wedge work).
- **Clients fall back to building locally, not to an error.** The connection
  relay child, on seeing its control socket to the coordinator close (EOF),
  takes the **same in-process build path O1 defines as the no-coordinator
  fallback** (spike §5.1, §1.1). If several attached clients fall back at once,
  their re-runs **coalesce on the output `PathLocks`** (spike §1.3) — exactly one
  rebuild happens, not N. The client loses live fan-out for that build but still
  gets a correct `BuildResult`.
- **Correctness is guaranteed by the existing lock/validity logic** (spike §2.2):
  a restarted coordinator plus output `PathLocks` and output-validity checks
  **never double-builds or corrupts**; the worst case is strictly today's
  behaviour (work coalesced by locks, no fan-out) until the build finishes.

This makes O1's deferred "daemon-restart survival" question answered as **no, not
in v1** — and bounds the cost of that answer to *recomputing* an in-flight build,
never to incorrectness.

### 2. Rejected alternatives

- **Persistent registry + re-adoption of running build subprocesses.** Rejected
  *for v1*, deferred as evidence-gated future hardening (see §5). It requires
  builders to **outlive** the coordinator (the opposite of the `dieWithParent`
  rule above), reintroducing the orphan-lifecycle, pid-reuse, and stale-handle
  bookkeeping the spike rejected M2 (shared memory) for (spike §2.3), plus a
  re-attachable handle to a running builder's live log stream. That is a large
  mechanism for a **rare** event (the coordinator is small and supervisory by
  O1/M1 design) — poor ROI until crashes are shown to matter operationally.
- **Leave builds running orphaned after coordinator death (no death signal).**
  Rejected: an orphan holds output `PathLocks` and writes log frames into a dead
  pipe (`EPIPE`), so it wedges/wastes work that nobody can collect a
  `BuildResult` from — worse than killing and letting `PathLocks`-coalesced
  re-run reclaim it.
- **Error attached clients when the coordinator dies.** Rejected: the local-build
  fallback already exists (O1) and is strictly better — the client still gets its
  output instead of a spurious failure.

### 3. What it commits

- Coordinator state is **in-memory, not persisted**, in v1.
- Builders are forked **`dieWithParent`** / in a coordinator-killable cgroup, so
  a coordinator crash reliably releases their `PathLocks`.
- The relay child's **control-socket-EOF → build-locally** fallback (the same
  branch as "no coordinator present").
- No public-wire surface; nothing here is a back-compat promise.

### 4. Residual risk and the guarding test

- **Risk:** a long build (LLVM/kernel) in flight at crash time is discarded and
  recomputed. Bounded by crash rarity; if it proves painful, that is the trigger
  to build the deferred re-adoption path (§5), not to block v1 on it.
- **Risk:** "thundering reconnect" — many attached clients fall back at once.
  Bounded: output `PathLocks` serialize them into **exactly one** rebuild.
- **Guarding test:** kill the coordinator mid-build with two clients attached and
  assert (a) no partial/corrupt output is ever observed as valid; (b) both
  clients still get a correct **successful** build via local fallback, with
  **exactly one** rebuild (marker/counter); (c) **no builder process survives**
  the coordinator (the `dieWithParent`/cgroup tie); (d) the daemon parent
  **respawns** a coordinator for a subsequent independent build.

### 5. Owner + follow-up

- **Owner:** libstore/daemon maintainer (build/`Worker` lifetime + daemon
  process model).
- **Follow-up:** implement the `dieWithParent`/cgroup tie and the relay
  EOF→local-fallback in the spike prototype (spike §4.1) so crash-degrade is
  exercised, and the §4 guarding test added; file **persistent registry +
  re-adoption** as a *separate, evidence-gated* hardening item (its own small RFC
  if/when coordinator-crash frequency justifies it), explicitly **not** a Phase 3
  prerequisite. RFC §4.3.5 records the registry-interface seams (the lease/CAS
  shape and the live-registry / durable-reuse-cache split) that keep this future
  hardening an additive extension rather than a redesign.

---

## Operational decision O3 — Lazy-spawn lifecycle (spike §6 Q8)

> Operational, **no public-wire** surface. Mostly *defanged by O1* (supervised
> spawn is canonical; lazy spawn is the fallback), so this only specifies the
> fallback's election, idle-exit, and respawn behaviour.

### 1. Decision

- **Within one daemon there is no election.** The single `daemonLoop` parent is
  the sole spawner (O1) and spawns the coordinator **lazily, on the first
  capability-negotiated build request** — not at daemon start — so installs that
  never use cross-client dedup pay nothing.
- **Election primitive for the fallback / multi-daemon case: the socket path
  itself.** A would-be coordinator claims it by taking an exclusive sidecar lock
  (`coordinator.socket.lock` via `flock`/`O_EXCL`) and then `bind()`ing
  `$NIX_STATE_DIR/coordinator.socket`; everyone else `connect()`s. At most one
  lock-holder binds, so the race is resolved atomically by the kernel — no
  bespoke election protocol.
- **Stale socket reclaim.** A `connect()` that fails `ECONNREFUSED`/`ENOENT`
  (coordinator crashed, socket left behind — O2) triggers a claim attempt (take
  lock, `unlink` the stale socket, `bind`), so exactly one child respawns.
- **Idle-exit (lazy mode).** A lazily-spawned coordinator idle-exits after a
  **configurable grace period (default proposal 10 min)** with zero builds and
  zero subscribers, so transient/test setups do not leak a process. Under daemon
  supervision idle-exit is optional — the parent reaps it on daemon stop.
- **Decline-and-respawn.** A coordinator that has begun shutdown stops accepting
  and answers a late connector with `GOING_AWAY` (or the child simply sees EOF);
  the child re-runs the election and retries, bounded. This closes the
  "idle-exit between `connect()` and first request" race (spike §3.1).

### 2. Rejected alternatives

- **Spawn unconditionally at daemon start.** Rejected: wastes a coordinator
  process on the majority of installs that never dedup; lazy-at-first-capability
  is free when unused.
- **A dedicated election / lock-manager daemon.** Rejected: the socket `bind` +
  lockfile is sufficient and introduces no new long-lived component.
- **No idle-exit in lazy mode.** Rejected: leaks an orphaned coordinator in
  transient and test setups.

### 3. What it commits

The lockfile+`bind` election; lazy-at-first-capability-request spawn; a
configurable idle-exit grace; the `GOING_AWAY`/decline-and-respawn handshake. No
public-wire surface.

### 4. Residual risk and the guarding test

- **Risk:** lock/`bind`/`unlink` reclaim has a narrow window under heavy crash
  churn; bounded retries plus the O2 `PathLocks` floor keep it correct, never
  corrupt.
- **Guarding test:** (a) N children issue their first capability-negotiated
  request simultaneously → **exactly one** coordinator and one socket; (b) force
  idle-exit then connect → decline-and-respawn yields a fresh coordinator and the
  build succeeds; (c) kill the coordinator leaving a stale socket → the next
  child reclaims (`unlink`+`bind`) and succeeds.

### 5. Owner + follow-up

- **Owner:** libstore/daemon maintainer.
- **Follow-up:** implement the election + idle-exit + decline-and-respawn in the
  spike prototype (spike §4.1), replacing the §3.1 sketch.

---

## Operational decision O4 — Coordinator throughput posture (spike §6 Q7)

> Operational, **no public-wire** surface. A *posture* decision plus a
> measurement gate — it deliberately does not pre-commit a scaling design.

### 1. Decision

**v1 ships a single-threaded coordinator event loop** (the §3 design).
Sharding/multithreading is **deferred and gated on the prototype's throughput
measurement** (spike §4.2). Rationale and guard-rails:

- The coordinator's central per-frame cost is only "append to replay buffer +
  persisted-log writer + write to each subscriber fd"; the **heavy per-client
  relay already lives in the connection children** (spike §3.4), so the
  central serialization point is far lighter than "all log traffic, fully
  processed, in one thread."
- **The natural shard axis, if needed, is the build key:** builds are
  independent and the registry is the only shared structure, so a later design
  can drain build→subscriber fan-out on a per-build worker / thread pool behind
  the single-threaded registry without a redesign. This is the same axis a
  persistent/distributed registry shards on (RFC §4.3.5 seam 5), so the
  sharded-coordinator and persistence extensions align on one key.
- Because the coordinator is **below the wire** (spike §5.1), moving from one
  loop to a sharded design later is an **internal** change with no wire or
  back-compat impact — so fixing the v1 posture now forecloses nothing.

### 2. Rejected alternatives

- **Build a sharded/multi-threaded coordinator now.** Rejected: premature
  without load data, and it adds concurrency hazards to the *trust-critical*
  component before its real load is known.
- **Cap concurrent builds at the coordinator to dodge throughput.** Rejected:
  reintroduces client-side-style gating that fights **G6** (elastic backends),
  the very thing the redesign exists to stop.

### 3. What it commits

v1 = single event loop; the spike §4.2 throughput measurement as the gate; the
build-key shard axis named for a future sharded design if measurement demands
it. No public-wire surface.

### 4. Residual risk and the guarding test

- **Risk:** a single loop becomes the bottleneck under extreme build/subscriber
  fan-out. **Guard:** the spike §4.2 measurement (coordinator CPU + per-frame
  fan-out cost under dozens of parallel builds × several subscribers) is the
  trigger to shard, with a provisional ceiling recorded to revisit.

### 5. Owner + follow-up

- **Owner:** libstore/protocol maintainer.
- **Follow-up:** record the throughput numbers from the spike prototype; only if
  they exceed the provisional ceiling, design the per-build-key sharded loop.

---

## Operational decision O5 — Replay cap default and truncation UX (RFC Q1 remainder, spike §6 Q4)

> Operational/UX; the replay buffer's *location* was already decided (coordinator
> memory, spike §3.5). This fixes only the **cap and the truncation
> presentation**. The cap is coordinator-internal; the truncation marker is an
> ordinary log frame, so there is **no public-wire** surface.

### 1. Decision

- **Byte cap, default 4 MiB** of structured frames per build (matching the spike
  §3.5 proposal), **configurable** (a `…-replay-cap` setting).
- **Over the cap, switch to head + tail:** retain the first **~1 MiB head** and
  the last **~3 MiB tail** (tunable within the cap) with an explicit synthetic
  frame `…N frames / M bytes truncated…` at the seam. The head preserves the
  configure / early-failure context that explains most failures; the tail
  preserves the live edge a late joiner is about to follow.
- **Late-joiner UX:** replayed frames carry `replayed=true` (already in spike
  §3.5 / RFC §4.3.1) so clients render them dimmed/collapsed; the truncation
  marker is a distinct, visible frame.
- **Post-build:** the buffer is discarded; any further joiner gets the **whole**
  log via the persisted-log fetch path (Phase 0 `QueryBuildLog`). The cap bounds
  only *live* memory — never the durable log.

### 2. Rejected alternatives

- **Unbounded full-log buffer.** Rejected: coordinator OOM on kernel/LLVM-scale
  logs.
- **Tail-only.** Rejected: drops the configure-phase context that diagnoses most
  failures.
- **A line/frame-count cap instead of bytes.** Rejected: one pathologically long
  line is unbounded; bytes are the safe currency.

### 3. What it commits

A byte cap (default 4 MiB), the head+tail split with a truncation-marker frame,
`replayed=true` tagging, and post-build handoff to the persisted log. No
public-wire surface.

### 4. Residual risk and the guarding test

- **Risk:** chosen head/tail split discards a relevant middle section. Bounded:
  the full log is always fetchable post-build via `QueryBuildLog`.
- **Guarding test:** a long-build replay test asserting head + tail + the
  truncation marker, that a late joiner's `replayed` prefix + live tail equals
  the originator's stream minus the truncated middle, and that coordinator memory
  stays bounded under a multi-hundred-MiB log.

### 5. Owner + follow-up

- **Owner:** libstore/protocol maintainer (+ a UX reviewer for the marker
  rendering).
- **Follow-up:** tune the cap and the head/tail split from the prototype's real
  logs; wire the configurable setting.

---

## Operational decision O6 — `QueryActiveBuilds` privacy default (RFC Q5, spike §6 Q6)

> A **policy** decision (chosen by the project) about default visibility; it
> reuses Blocker 1's per-observable authorization and adds no new authz
> mechanism. Filtered in one chokepoint (the coordinator's `QUERY_ACTIVE`
> handler, spike §3.7.3).

### 1. Decision

**Default: an untrusted caller sees only the builds it is itself authorized to
build, and nothing else** — no other tenant's names, keys, per-build detail, or
existence signal, and **no aggregate count**. An operator **may opt in** (a
setting, e.g. `query-active-builds-aggregate = true`) to additionally expose an
**anonymized aggregate in-flight count** (a single scalar: total builds running),
carrying no keys/names/per-build detail. Trusted callers retain full
introspection. Rationale:

- Default-off keeps the **no-existence-oracle** property of Blocker 1 intact: an
  aggregate count is a coarse cross-tenant load side-channel, so it is exposed
  only on an explicit operator opt-in, never by default.
- When enabled, the count is the *only* cross-tenant-derived datum and carries no
  per-build identity, so it can never become a per-key existence oracle (you
  cannot probe "is *this* drv building" from a global integer).

### 2. Rejected alternatives

- **Always expose an aggregate count.** Rejected: leaks coarse cross-tenant
  load by default, weakening the conservative posture for a monitoring nicety.
- **Never offer a count, even as opt-in.** Rejected: removes a legitimate
  operator monitoring affordance with no security gain over default-off.

### 3. What it commits

Default filtering = own-authorized-builds-only; an opt-in setting for an
anonymized aggregate count; both enforced at the single `QUERY_ACTIVE` chokepoint
reusing Blocker 1's per-observable authz. `QueryActiveBuilds` is already a
version-gated op (RFC §7); this fixes its **default policy**, not a new field
(the optional count is one scalar in the response).

### 4. Residual risk and the guarding test

- **Risk:** even an anonymized count reveals that *some* activity exists.
  Bounded: off by default, no identity, operator-chosen.
- **Guarding test (extends Blocker 1's existence-oracle test):** an untrusted
  caller with the setting **off** sees only its own builds and no count; with it
  **on** sees its own builds plus a count but **no** other-tenant names/keys; and
  a `QUERY_ACTIVE` for a specific unauthorized in-flight key is byte- and
  timing-identical to one for a non-existent key.

### 5. Owner + follow-up

- **Owner:** security reviewer + libstore/protocol maintainer.
- **Follow-up:** implement the default filter + the opt-in aggregate setting in
  Phase 5 (`QueryActiveBuilds`), with the trust test above.

---

## Operational decision O7 — Elastic-capacity advertisement (RFC Q6)

> A **Phase 6 / G6** decision. It touches the wire only additively (a negotiated
> capability flag, gated like every other new capability, RFC §7) and is
> **strictly opt-in** so existing `/etc/nix/machines` files are never silently
> overcommitted.

### 1. Decision

Support **both** signals, with operator config taking precedence:

- **(a) Handshake advertisement.** A builder may advertise an
  **"elastic / self-scheduling"** capability during the protocol handshake (a
  negotiated capability alongside the version bump, RFC §7), so a backend like
  nixbuild.net "just works" without per-user config.
- **(b) Per-machine operator field.** An operator may force the behaviour via a
  new field in `/etc/nix/machines` / store config (a `self-scheduled`/`elastic`
  flag), which **overrides** the advertisement in both directions (force-on for a
  non-advertising backend; force-off to correct one that mis-advertises).

When either signal selects elastic, the distributed-build hook **stops gating on
local per-slot file locks** for that builder (`build-remote.cc:151-177`) and
treats `maxJobs` as a concurrency **hint**, not a hard cap.

**Fixed constraint (RFC §4.7.1):** this is **strictly opt-in**. Absent both
signals, `maxJobs` keeps today's hard-cap semantics, so existing machines files
are unchanged and fixed-size builders are never silently overcommitted.

### 2. Rejected alternatives

- **Handshake only.** Rejected: no operator override — can't force a
  non-advertising backend, can't correct a mis-advertising one, no manual
  control.
- **Config only.** Rejected: every elastic backend must be hand-configured by
  every user, ignoring that capable backends can simply advertise.
- **Change `maxJobs` to a hint by default.** Rejected (RFC §4.7.1):
  silently overcommits fixed-size machines operators rely on.

### 3. What it commits

A negotiated handshake **capability flag** (version-gated) **+** a per-machine
config **field**; **operator config overrides advertisement**; default-off
hard-cap `maxJobs` semantics unchanged. Lands in **Phase 6**.

### 4. Residual risk and the guarding test

- **Risk:** a backend advertises elastic but is actually fixed-size. Mitigated by
  the operator force-off override and by the whole behaviour being opt-in.
- **Guarding test:** (a) a builder with **neither** signal keeps hard-cap
  `maxJobs` (no slot overcommit, today's behaviour); (b) an **advertised**-elastic
  builder → the hook does not gate on slot locks and treats `maxJobs` as a hint;
  (c) operator **force-off** overrides an advertised-elastic builder; (d) operator
  **force-on** enables a non-advertising builder.

### 5. Owner + follow-up

- **Owner:** libstore/protocol maintainer (+ `build-remote`/`machines` owner).
- **Follow-up:** Phase 6 — implement the handshake capability, the machines-spec
  field, the precedence rule, and the slot-gating change at
  `build-remote.cc:151-177`.

---

## Cross-cutting answers (all three blockers)

- **Identical Build Session wire for single-process and stock-daemon backends?**
  Yes. Blocker 1's key/auth and Blocker 2's cancel matrix are entirely
  coordinator-internal (spike §3, §5.1/§5.3) — a client cannot tell whether
  `STDERR_*` frames, `deduplicated`, and the `BuildResult` came from a
  single-process backend's in-memory broadcaster or a stock daemon's
  coordinator-relayed child. Blocker 3's fields are negotiated identically over
  the same serve serializer regardless of backend.
- **Trust model and no-flag-day preserved?** Yes. Blocker 1 *is* the trust
  decision (per-observable, re-derived, existence-oracle-proof). Blocker 2 adds
  no wire surface. Blocker 3 is additive and `min()`-gated in both directions; the
  2.9 layout is frozen only after the back-compat matrix is proven by golden
  tests.
- **Minimum to freeze now vs. behind an unstable version?** *Freeze now:* the
  **build-key definition** and **per-observable authorization rule** (Blocker 1)
  — they define what "the same build" means and who may observe it, and are
  expensive to change later. *Freeze with Hydra:* the serve diagnostic core **diagnostic
  core** (`logRef`, `failurePhase`, `exitCode`, `logTail`, `QueryBuildLog`).
  *Stay unstable:* `builderId`, `deduplicated`, the RFC §4.4
  failure-classification fields (transient/failure-class/resource hint), and the
  CA `resolving`-merge wire details, all bound to the still-spiking Phase 3
  coordinator / elastic-backend design. Blocker 2 freezes
  *no* wire (coordinator-internal) but its table is agreed before Phase 3 code.
- **Test that proves each (per RFC §9):** B1 → trust tests (existence-oracle,
  CA-merge re-auth, key-spoof); B2 → the `build-dedup-cancel` matrix; B3 →
  characterisation/golden serializations + the 2.8-reads-2.9 back-compat test +
  the serve-path `nix log` functional test.

## Decision summary / owners

| Blocker | Decision in one line | Freezes now? | Owner | Follow-up |
|---|---|---|---|---|
| 1 — CA key-merge trust | Key = hash of client-resolved drv (coordinator-derived); per-observable authz re-derived per-subscriber against the resolved key, before registry lookup; no existence oracle; `resolving` merge re-authorizes at promotion | **Yes** (key + authz rule) | Security reviewer + libstore/protocol | ✅ CA `resolving`→promote prototype + trust tests **done** (T1–T3 green, Workstream B; [validation.md](./remote-build-protocol-redesign.validation.md)) |
| 2 — Refcounted cancel | Lifetime = refcount + explicit-root only; no originator privilege; per-subscriber timeout under a max envelope; keep-failed = OR; cancel = scoped unsubscribe-with-error | No wire (coordinator-internal); table agreed pre-Phase-3 | libstore/protocol | ✅ `hasRootReasonToContinue` + per-subscriber deadlines + `build-dedup-cancel` matrix **done** (C-a…C-f green, Workstream C) |
| 3 — Hydra / serve diagnostic core | Freeze diagnostic core (`logRef`, phase/exit/tail, `QueryBuildLog`); defer `builderId`/`deduplicated` to unstable; bump to 2.9 only on the four freeze criteria | Diagnostic core, gated by Hydra sign-off | Hydra queue-runner maintainer (TBD) + libstore/serve-protocol | ⏳ characterisation tests **done** (D2/D3.3 green, Workstream D); Hydra coordination thread + soak on unstable still owed (D3.1/D3.2/D3.4, external) |
