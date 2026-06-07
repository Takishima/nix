# Phase 3 spike decision — cross-process build coordination (§4.3.3)

> **Status:** decision recorded; gates Phase 3 wire surface (now unblocked).
> **Decision:** **Mechanism 1 — the coordinator process.**
> **Parents:**
> [`../remote-build-protocol-redesign.spike.md`](../remote-build-protocol-redesign.spike.md)
> (§2 mechanism evaluation, §3 interface) ·
> [`../remote-build-protocol-redesign.decisions.md`](../remote-build-protocol-redesign.decisions.md)
> (Blocker 1 trust, Blocker 2 refcounted-cancel, O1–O5) ·
> [`../remote-build-protocol-redesign.validation.md`](../remote-build-protocol-redesign.validation.md)
> (F-INT freezable now) ·
> [`./workstream-a-coordinator/`](./workstream-a-coordinator/) (running prototype).

## 0. What this document is

§4.3.3 of the RFC flags the cross-process coordination mechanism as a **design
spike that must precede freezing any Phase 3 wire surface**, "because the
mechanism bounds what session re-attach and `QueryActiveBuilds` can promise and
what the Phase-3 wire must look like." This doc records the spike *outcome* — the
mechanism chosen, why, what the prototype proved, and what that licenses Phase 3
to build — so the decision is auditable in one place. It is the
short-decision-doc deliverable §8/§4.3.3 asks for; the long-form evaluation lives
in `remote-build-protocol-redesign.spike.md` §2, and the running evidence in
`prototypes/workstream-a-coordinator/`.

## 1. The decision

For the **stock fork-per-connection `nix-daemon`**, the Build Registry, replay
buffers, and subscriber refcounts are owned by a **long-lived per-store
coordinator process** (Mechanism 1, spike §2.2/§2.5). Forked connection children
become thin relays: on a build request a child does a *start-or-attach* RPC to
the coordinator over a peer-cred-verified Unix socket, then relays the
coordinator's log frames down its own client socket and forwards
cancellation/disconnect. The coordinator runs the real `Worker` and `fork()`s the
builder per build, **exactly as the daemon does today**, so builder-crash
isolation is preserved.

Single-process backends (a single `RemoteStore` driver, nixbuild.net-style
services) and the distributed orchestrator+pods class (§4.3.4) need **none** of
this stock-daemon coordinator: they own an in-address-space (or in-orchestrator)
registry directly. Phase 3 therefore lands the registry **first for the
single-process case** (no coordinator) and only later for the stock daemon via
the coordinator.

## 2. Why Mechanism 1 (and not 2 or 3)

The criteria that had to be *confronted, not hand-waved* (spike §2.1) are **fault
isolation, crash cleanup, and cross-tenant log leakage**. Scored against those
(spike §2.5):

| Criterion | M1 Coordinator | M2 Shared memory | M3 Single-process daemon |
|---|---|---|---|
| Fault isolation | builder crashes stay contained (still forked); only coordinator-`Worker` bugs are a shared domain (≈ M3 on that one axis, far smaller code) | **worst** — shared failure domain incl. ring corruption | **strictly worse than today** — one `Worker` bug kills every client |
| Crash cleanup | one place; coordinator death **safe-degrades** to today (PathLock-coalesced local builds) | orphan segments, robust-futex, pid-reuse races | all-or-nothing |
| Backpressure | per-socket, **drop-slow-client** (build never throttled) | lossy ring or throttles the build | easy but shares the daemon heap |
| Security / trust | **existing trust code**, authorize-before-subscribe in one place | the segment *is* the cross-tenant leak | existing trust code |
| Impl. blast radius | medium, **throw-away prototype** | deceptively large, hard to prototype | largest, not throw-away (its own multi-quarter RFC) |
| Wire promise (outlive conn) | yes (coordinator-uptime-bounded) | only by converging toward M1 | yes |

**M1 is the only option that** (a) preserves today's *builder-crash* isolation by
keeping builders forked while adding a small, supervisory coordinator; (b)
concentrates the genuinely hard parts — refcounted cancellation, replay-buffer
ownership, `QueryActiveBuilds`, trust filtering — in **one place running the
existing trust code**; (c) keeps the client-facing wire **identical**, so the
Phase 3 framing is decoupled from the mechanism (no flag day, spike §5.1); and
(d) yields a **genuinely throw-away prototype** (a sidecar + a flagged relay
path). M2 trades the coordinator for a pile of shared-memory crash-recovery
footguns that *reintroduce* a coordinator under another name with a worse trust
story. M3 is the cleanest end-state on paper but the worst on the axis the
reviewer cares most about (isolation), and is far too large to be a spike.

The honest caveat (spike §2.2, carried forward): a bug in the coordinator's *own*
`Worker`/scheduling logic is a shared crash domain across connections — the same
class M3 carries. M1's win there is that it is a far smaller, more contained piece
of code, and the safe-degrade floor (O2) bounds the *consequences*.

## 3. What the prototype proved (start-or-attach + log fan-out)

`prototypes/workstream-a-coordinator/` is the throw-away prototype the spike asks
for: ~900 lines of standalone C++ modelling the **real** daemon/relay/coordinator
topology — real `fork()`-per-connection, a real separate coordinator process,
real Unix sockets, **real `SO_PEERCRED`** auth, real non-blocking single-threaded
fan-out, real `PR_SET_PDEATHSIG` orphan-prevention, real `flock`-coalesced
fallback. `make check && make check-b && make check-c` is green. The acceptance
matrix it discharges (validation.md Workstream A/B/C):

| ID | Asserts | What it licenses |
|---|---|---|
| **A-dedup** | two concurrent clients on one key → exactly **one** build (`counter == 1`) | start-or-attach coalescing (§3.3) |
| **A-replay** | late joiner's `replayed=true` prefix + live tail == originator's full stream, no gap/dup at the seam | replay buffer in coordinator memory, head+tail cap (RFC Q1, O5) |
| **A-backpressure** | a stalled client is **demoted**, the build still completes | build decoupled from slow clients (§3.6) |
| **A-sockauth** | uid ≠ daemon-uid refused at `SO_PEERCRED` before any `sessionAuth` is read | authenticate-the-socket linchpin (§3.7.1) |
| **A-crash** | kill coordinator mid-build → no orphan builder; relays fall back to **exactly one** rebuild (PathLock floor); next build respawns | safe-degrade posture (O2) |
| **A-spawn** | N racing first-requests elect **exactly one** coordinator + socket; idle-exit; respawn | lazy-spawn election (O3) |
| **B / T1–T3** | existence-oracle-free denial; CA resolve→promote→**re-auth**; asserted-key spoof rejected | Blocker 1 trust (gates F-WIRE) |
| **C-a…C-f** | the full refcounted-cancel matrix | Blocker 2 (no originator privilege; per-subscriber-deadline max-envelope; keep-failed OR; scoped cancel) |

This is the evidence behind RFC **Q0** (coordinator) and **Q1** (replay buffer in
coordinator memory, head+tail cap), and it discharges gate **F-INT** (the Phase 3
*internal* coordinator interface is freezable). What graduates from the prototype
is the **interface** (spike §3.2) and the *evidence that it holds* — none of the
prototype code merges into `libstore`/`daemon`.

## 4. What this licenses Phase 3 to build (and the seams it must keep)

With M1 chosen and F-INT validated, Phase 3 implements the Build Registry **first
for single-process backends** (an in-address-space registry — no coordinator),
then for the stock daemon via the coordinator. The single-process registry is the
same component the coordinator will host; it must therefore be written to the
**interface**, not the transport, so the coordinator is a later host rather than a
rewrite. The normative seams (RFC §4.3.5, §8.1) the registry component bakes in:

1. **Key on the resolved derivation** — the store path of the resolved drv
   (Blocker 1, guardrail §8.1 #1). Never an output path or the input-addressed
   drv path. The resolved-drv hash covers `system` + required features, so the
   key is architecture-safe (never coalesces x86 with aarch64).
2. **Lease/CAS-shaped interface from day one** (§4.3.5 seam 2). v1 gets atomicity
   from a single event loop, but `lookupOrCreate` is written as compare-and-swap
   + a renewable, fenceable lease, so a future persistent/distributed registry is
   a drop-in, not a rewrite.
3. **Pluggable authorization policy** (guardrail §8.1 #2, §4.3.4). The registry
   programs against an `AuthPolicy` interface — peer-cred for the local
   coordinator, mTLS/identity for a network control plane — never an inlined
   `SO_PEERCRED`/same-host assumption at a call site. Authorization runs
   **before** the registry is consulted (Blocker 1: no existence oracle).
4. **Refcounted cancellation even when the count is always 1** (Blocker 2,
   guardrail §8.1 #3). Detach routes through `unsubscribe`, never an
   unconditional "client disconnect ⇒ kill the build."
5. **Two layers named separately** (§4.3.5 seam 3): the volatile *live coalescing
   registry* (what v1 ships) is distinct from the durable *reuse cache*
   (substitution / CA realisations), so persistence can be added to either
   independently.
6. **`deduplicated`/`builderId` are append-after the frozen diagnostic core,
   under the unstable gate** (Blocker 3, guardrail §8.1 #7). Phase 3 *defines*
   their semantics (gate H3); it does **not** bump `SERVE_PROTOCOL_VERSION`.

## 5. Landed (this and the prior Phase-3 session)

- **In-address-space Build Registry** (`build/build-registry.{hh,cc}`) — the
  interface + in-memory implementation with the full Blocker-2 cancel matrix,
  replay buffer, fan-out, `deduplicated`, and unit tests.
- **Stock-daemon coordinator + relay** (`build/build-coordinator.{hh,cc}`) — a
  separate lazily-spawned per-store coordinator that hosts the registry, runs
  builds in forked children, fans their log out, and replays to late joiners;
  reached from the daemon by a single additive branch in the `BuildDerivation`
  handler (expand/contract), gated by the **`build-coordinator` experimental
  feature** (the planned contraction's first half), with the socket at
  `$stateDir/coordinator.socket` (O1, `NIX_BUILD_COORDINATOR_SOCKET` override).
  Hardened against an oversized-record allocation DoS on the control socket.
- **Functional verification** — `build-dedup-coordinator.sh`: two concurrent
  `ssh-ng://localhost` builds of the same resolved derivation coalesce to **one**
  build (both clients observe the same builder-process token), the late joiner
  receiving the pre-attach log via **replay**. `build-dedup-cancel.sh`: the
  refcounted-cancel case C-a — the originator is killed while a joiner remains,
  and the build **continues** to the joiner's completion (no originator
  privilege, Blocker 2).

## 6. Deferred (evidence-/design-gated, not on the critical path)

- **Finish the contraction**: the daemon `--coordinator` supervised role (O1)
  and collapsing the duplicated direct-build branch once the coordinator is the
  proven default.
- **Broaden the relay** beyond `BuildDerivation` to `BuildPaths` /
  `BuildPathsWithResults` (resolve each derived path to its key) so top-level
  `ssh-ng://` builds dedup too, not only hook-offloaded ones.
- **Cross-user trust hardening (Blocker 1)**: the coordinator should recompute
  the build key from the received drv (T3) and re-authorize every subscriber
  against the resolved key via a real `BuildAuthPolicy` (replacing `AllowAll`),
  needed once the per-machine coordinator spans users. Today the daemon's
  pre-relay trust check plus same-uid peer-cred cover the single-user gate.
- **`derivation-building-goal.cc` log-fidelity**: the relay currently re-emits
  the coordinator's frames as plain log lines; structured activity framing
  (guardrail §8.1 #5, the per-build top-level activity) is a refinement.
- **CA `resolving`→promote** in production (modelled as a timer in the prototype,
  proven by Workstream B).
- **Persistent live-registry** (survive-coordinator-restart, O2's deferred
  re-adoption) and **`QueryActiveBuilds` UX / `nix` subcommand** (Phase 5).
- **Session re-attach after a fully dropped connection** (Phase 6).
