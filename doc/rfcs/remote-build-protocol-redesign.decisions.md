# Decisions: the three blockers before the Phase 3 / serve-3.0 wire freeze

| | |
|------------------|------------------------------------------------|
| **Status**       | Decision record (converged) — unblocks Phase 3 wire design |
| **Parent**       | [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md) |
| **Spike**        | [`remote-build-protocol-redesign.spike.md`](./remote-build-protocol-redesign.spike.md) |
| **Reviews**      | [`*.review.md`](./remote-build-protocol-redesign.review.md), [`*.spike.review.md`](./remote-build-protocol-redesign.spike.review.md) |
| **Resolves**     | RFC Q2 (cancel matrix), Q3 (CA resolution timing), Q4 (Hydra field set), Q7 (reuse-key vs. trust); spike §5.4.1–.2, §6 Q2/Q3 |
| **Working group**| libstore/protocol maintainers, a Hydra maintainer, a security reviewer |

> This document **converges on decisions** — not options — for the three issues
> that gate freezing any new wire surface for remote-build dedup/attach (RFC
> Phase 3) and the extended `BuildResult` (serve 3.0). Everything here is bounded
> by the three invariants the RFC and spike establish and that must not break:
> (1) the **trust model** (RFC §6) — dedup must never become a cross-tenant
> log/result/existence oracle; (2) **no Hydra/Nix flag day** — every change is
> additive and version-gated via the `min(client,server)` handshake
> (`serve-protocol-connection.cc:8-33`); (3) **below-the-wire** — the
> client-facing Build Session surface is identical whether the backend is a
> single-process/elastic service or the stock fork-per-connection daemon +
> coordinator (spike §5.1, §5.3).
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

## Blocker 3 — The Hydra field set & serve-3.0 freeze (RFC Q4, §7, spike §5.1)

### 1. Decision

Split the extended `BuildResult` into a **stable diagnostic core** and a
**deferred dedup/fleet-observability set**, and freeze only the former for serve
3.0. The **stable core** (the smallest set that lets `hydra-queue-runner` retire
its out-of-band log handling and report failures structurally) is: `logRef` (the
resolved drv path the builder asserts it persisted the log under, the key
`LogStore::getBuildLog` already uses), structured failure detail
`failurePhase` + `exitCode` + `logTail`, **plus a new `QueryBuildLog` serve
operation**. The **deferred set** — `builderId` and `deduplicated` — stays behind
an **unstable/experimental serve version** because its semantics are defined by
the Phase 3 coordinator/dedup design, which is explicitly *not* frozen; freezing
it now would overcommit to dedup semantics still in spike. The absolute
**minimum to unblock Phase 1 without overcommitting is `logRef` +
`QueryBuildLog`** (the two that let Hydra fetch the persisted log instead of
capturing it inline); `failurePhase`/`exitCode`/`logTail` are the recommended,
low-risk companions that also deliver fail-loud (G8) for non-Hydra serve clients.
Serialization **extends the existing conditional, version-gated serve
serializer ladder** (`serve-protocol.cc:29-96`, the `>= {2,3}/{2,6}/{2,8}`
pattern): new fields are appended **after** the 2.8 `builtOutputs` block under a
`>= {3,0}` guard, in **binary length-prefixed** form (not JSON — JSON is used
only for the pre-existing 2.6 realisation back-compat hack), so a 2.8 reader
stops before them and **no existing field changes meaning**; the CA realisation
fields Hydra already consumes (`std::map<OutputName, UnkeyedRealisation>` at 2.8)
are untouched. `QueryBuildLog` is a **new `Command` enum value (`= 10`, the next
free number after `AddToStoreNar = 9`)**, never sent unless the negotiated
version supports it. **`SERVE_PROTOCOL_VERSION` is NOT bumped to `(3 << 8 | 0)`
until the freeze criteria below are met**; until then the fields live behind an
explicitly-unstable provisional gate whose byte layout is *not* a back-compat
promise.

**Freeze criteria for serve 3.0 (all must hold):**
1. A **named Hydra maintainer** has reviewed and signed off on the exact frozen
   field set and byte order.
2. A `hydra-queue-runner` branch (a) consumes `QueryBuildLog` + the structured
   log frames (§4.2) to **drop its out-of-band log capture**, and (b) reads the
   extended `BuildResult`, both validated against a new-Nix builder.
3. **Golden/characterisation serialization tests** (`src/libstore-tests`,
   `src/json-schema-checks`) prove round-trip at 2.8 and 3.0 **and** that a 2.8
   peer ignores 3.0 fields — the full back-compat matrix, both directions.
4. The field set has been carried on the **unstable version for ≥1 release
   cycle** with no layout change.

Only then bump `SERVE_PROTOCOL_VERSION` to `(3 << 8 | 0)` and treat the layout
as frozen.

### 2. Rejected alternatives

- **Freeze the full field set (incl. `builderId`/`deduplicated`) as 3.0 now.**
  Rejected: their meaning depends on the unfrozen Phase 3 coordinator/dedup
  design; freezing a byte layout for semantics still in spike risks a later
  Hydra/Phase-3-driven change breaking the very back-compat §7 promises — exactly
  the retroactive-break the RFC warns against (§7 final bullet, review §4).
- **JSON-encode the new fields.** Rejected: the serve protocol is binary
  length-prefixed; JSON is present only as a 2.6 realisation compat shim
  (`serve-protocol.cc:81-95`). New scalar/string fields (`exitCode`,
  `failurePhase`, `logRef`, `logTail`) serialize natively and cheaper as binary.
- **Bump `SERVE_PROTOCOL_VERSION` to 3.0 immediately to start using the
  fields.** Rejected: a shipped 3.0 layout is a permanent back-compat promise
  (§7). Prototype behind an unstable gate first; bump only at freeze.
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

- **Wire layout (frozen at 3.0):** the append-after-2.8-`builtOutputs`,
  `>= {3,0}`-gated, binary order of `logRef`, `failurePhase`, `exitCode`,
  `logTail` in the `BuildResult` serializer; and `QueryBuildLog` as
  `Command = 10`. Once 3.0 ships this is a back-compat promise.
- **Semantics:** `logRef` = the resolved drv path under which the builder
  asserts the log is persisted (`LogStore::getBuildLog` key); structured failure
  = phase/exit/tail as *fields*, not baked into the message string.
- **Deferred (explicitly NOT frozen):** `builderId`, `deduplicated` — carried on
  the unstable version, free to change until Phase 3 semantics settle.
- **Back-compat matrix (both directions, via `min()` handshake):**

  | Client | Server | Negotiated | Behaviour |
  |---|---|---|---|
  | old Hydra (≤2.8) | new Nix (3.0) | 2.8 | today's exchange; no new fields/op ✅ |
  | new Hydra (3.0) | old Nix (≤2.8) | 2.8 | Hydra falls back to out-of-band log capture; `QueryBuildLog` not sent ✅ |
  | new | new | 3.0 | full diagnostic core active ✅ |

### 4. Residual risk and the guarding test

- **Risk:** freezing 3.0 before Hydra's branch is proven could still miss a field
  Hydra needs. Mitigated by freeze criterion 2 (a working queue-runner branch is
  a *precondition* of the freeze) and criterion 4 (a soak cycle on the unstable
  version).
- **Risk:** `logRef` asserts persistence but a builder might not actually have
  retained the log (e.g. `keepLog` still suppressed on an older path). Mitigated
  by pairing `logRef` with the explicit "persisted" assertion (RFC §4.4) and by
  Phase 0 making `keepLog`/`verbosity` suppression conditional
  (`nix-store.cc:908-909`).
- **Guarding test (RFC §9 "Protocol characterisation tests"):** golden
  serializations of `BuildResult` at 2.3/2.6/2.8/3.0 and of a `QueryBuildLog`
  round-trip; an explicit **2.8-reads-3.0-bytes** test asserting the 2.8 reader
  consumes exactly the 2.8 fields and the negotiated-down peer never emits the
  3.0 tail; plus a functional test that `nix log` over the serve path returns the
  real log via `QueryBuildLog` (Gap A/§4.5).

### 5. Owner + follow-up

- **Owner:** **Hydra queue-runner maintainer** (sign-off authority on the frozen
  field set — to be named when the Hydra coordination thread opens; this record
  assigns the *role* and the gate), with a **libstore/serve-protocol maintainer**
  as the Nix-side counterpart who owns the serializer and the version bump.
- **Follow-up:** open the Hydra coordination thread (RFC Q4 says coordination is
  out of scope of the RFC itself); land the characterisation tests and the
  unstable-version implementation of the diagnostic core in Phase 1; **do not bump
  `SERVE_PROTOCOL_VERSION` to 3.0 until the four freeze criteria are met.**

---

## Operational decision O1 — Coordinator deployment model (spike §6 Q1, open-points §1.1)

> This is an **operational** decision, not a wire blocker: it concerns *how the
> coordinator process is deployed, owned, and reaped*, not the public protocol.
> It builds on the settled coordinator interface (spike §3) and does not reopen
> it. It changes **no public wire** (the coordinator is below the wire, spike
> §5.1), so it does **not** gate the Phase 3 / serve-3.0 freeze; it is recorded
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
  **intentionally deferred**: whether the coordinator must *survive* a daemon
  restart (persistent registry + re-adoption of running build subprocesses) is
  the **crash-recovery posture** still open in spike §6 Q5 / open-points §1.2;
  the canonical "parent owns it" model degrades to the `PathLocks` floor on
  restart, which O1 deems acceptable as the baseline.
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
  start-or-attach; settle the daemon-restart-survival question with spike §6 Q5
  (crash-recovery posture) rather than independently.

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
  3.0 layout is frozen only after the back-compat matrix is proven by golden
  tests.
- **Minimum to freeze now vs. behind an unstable version?** *Freeze now:* the
  **build-key definition** and **per-observable authorization rule** (Blocker 1)
  — they define what "the same build" means and who may observe it, and are
  expensive to change later. *Freeze with Hydra:* the serve 3.0 **diagnostic
  core** (`logRef`, `failurePhase`, `exitCode`, `logTail`, `QueryBuildLog`).
  *Stay unstable:* `builderId`, `deduplicated`, and the CA `resolving`-merge wire
  details, all bound to the still-spiking Phase 3 coordinator. Blocker 2 freezes
  *no* wire (coordinator-internal) but its table is agreed before Phase 3 code.
- **Test that proves each (per RFC §9):** B1 → trust tests (existence-oracle,
  CA-merge re-auth, key-spoof); B2 → the `build-dedup-cancel` matrix; B3 →
  characterisation/golden serializations + the 2.8-reads-3.0 back-compat test +
  the serve-path `nix log` functional test.

## Decision summary / owners

| Blocker | Decision in one line | Freezes now? | Owner | Follow-up |
|---|---|---|---|---|
| 1 — CA key-merge trust | Key = hash of client-resolved drv (coordinator-derived); per-observable authz re-derived per-subscriber against the resolved key, before registry lookup; no existence oracle; `resolving` merge re-authorizes at promotion | **Yes** (key + authz rule) | Security reviewer + libstore/protocol | CA `resolving`→promote prototype + trust tests |
| 2 — Refcounted cancel | Lifetime = refcount + explicit-root only; no originator privilege; per-subscriber timeout under a max envelope; keep-failed = OR; cancel = scoped unsubscribe-with-error | No wire (coordinator-internal); table agreed pre-Phase-3 | libstore/protocol | `hasRootReasonToContinue` + per-subscriber deadlines; `build-dedup-cancel` test |
| 3 — Hydra / serve 3.0 | Freeze diagnostic core (`logRef`, phase/exit/tail, `QueryBuildLog`); defer `builderId`/`deduplicated` to unstable; bump to 3.0 only on the four freeze criteria | Diagnostic core, gated by Hydra sign-off | Hydra queue-runner maintainer (TBD) + libstore/serve-protocol | Hydra coordination thread; characterisation tests; soak on unstable |
