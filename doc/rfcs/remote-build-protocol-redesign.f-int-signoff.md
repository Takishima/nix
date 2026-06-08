# F-INT sign-off dossier — the Phase 3 coordinator interface

| | |
|------------------|------------------------------------------------|
| **Status**       | Review request — engineering gates met, awaiting the libstore/daemon maintainer's freeze decision |
| **Gate**         | F-INT (the Phase 3 *internal* coordination interface) |
| **Owner of the decision** | libstore/daemon maintainer |
| **Companion to** | the [validation plan](./remote-build-protocol-redesign.validation.md) (F-INT checklist) and the [spike](./remote-build-protocol-redesign.spike.md) (§3, the interface) |

This document exists to make F-INT *actionable*. The validation plan assesses
the gate "freezable" and says the only thing left is a maintainer decision that
is "not a coding task"; this dossier packages everything that decision needs —
the exact surface being frozen, the invariants it commits to, the in-tree
evidence for each engineering gate, what the sign-off explicitly does **not**
cover, and the residual risks — so a reviewer can act without reassembling it
from four documents and the tree.

## 1. The decision requested

> **Freeze the `BuildRegistry` interface (seam 1) and the child↔coordinator
> control protocol as the coordination contract Phase 3 is built on.**

Concretely, tick these:

- [ ] The **operation set and semantics** of `BuildRegistry`
  (`src/libstore/include/nix/store/build/build-registry.hh`) are the contract:
  `startOrAttach` / `log` / `finish` / `unsubscribe` / `checkDeadlines` /
  `queryActive` / `keepFailedRequested` / `currentDeadline` / `isLive`.
- [ ] The **invariants** in §3 below are the ones we want to hold.
- [ ] The **child↔coordinator control protocol**
  (`src/libstore/build/build-coordinator.cc`) is an acceptable transport for
  that contract on the stock fork-per-connection daemon.
- [ ] The **deferrals** in §5 are correctly out of scope for *this* gate.

A reviewer who disagrees with any line should say which invariant or operation
to change; §7 explains why that is cheap to do even after this freeze.

## 2. What is being frozen (and what is not)

**Frozen by F-INT — the internal interface, below the client wire:**

- `BuildRegistry` — the abstract operations + invariants (seam 1). The v1
  in-memory implementation (`makeInMemoryBuildRegistry`,
  `src/libstore/build/build-registry.cc`) is *one* conforming implementation; a
  persistent / sharded / distributed registry must remain a drop-in.
- The coordinator's child↔coordinator control protocol: the length-prefixed
  tagged records (`START_OR_ATTACH`, `FRAME`, `RESULT`, `QUERY_ACTIVE`,
  `ACTIVE`) in `src/libstore/build/build-coordinator.cc`. This is **internal**,
  never the client wire — a daemon child still speaks ordinary worker-protocol
  `STDERR_*` to its client via the existing `TunnelLogger`.

**Not frozen by F-INT** — these have their own gates and are listed in §5: the
public Build Session wire (F-WIRE), the serve diagnostic core / version bump
(F-SERVE-DIAG), and the deferred serve fields (H3).

## 3. The invariants you are blessing — each mapped to in-tree code

| # | Invariant | Where it lives in-tree |
|---|---|---|
| I1 | **At most one live build per key per domain**; `startOrAttach` (lookup-or-create) is atomic w.r.t. the key. | `InMemoryBuildRegistry::startOrAttach` |
| I2 | **The key is the resolved-drv store path** — never a store output path nor the input-addressed `.drv`. Because the resolved drv covers `system` + required features, the key is **architecture-safe** (an x86 and an aarch64 build never coalesce). The registry never trusts a client-asserted key; the caller computes it from the drv it received. | `BuildRegistryKey`; the coordinator keys on the `BasicDerivation` it is sent (`handleStartOrAttach`) |
| I3 | **Authorize before consulting the registry** — a denied caller learns neither the log nor whether the build exists (no existence/timing oracle); denial is uniform (`std::nullopt`), not branched on existence. | `BuildAuthPolicy::mayBuild`, checked first in `startOrAttach` |
| I4 | **Refcounted cancellation.** A detach decrements the refcount; the build is cancelled (its `onCancel` fires) only at refcount 0 with no durable root. `Cancel` is a scoped unsubscribe-with-error — no new wire status. | `unsubscribe`, `DetachReason` |
| I5 | **A durable root is sticky** — once any subscriber registers `explicitRoot`, the build runs to completion even after every subscriber detaches; `--keep-going` is *not* a root. | `Build::rooted`, `hasRootReasonToContinue` |
| I6 | **Deadlines are per-subscriber under the max (most-generous) envelope**; a subscriber past its own deadline is detached with `TimedOut` without cancelling the build for others (strictest-wins is rejected as a cross-tenant DoS). | `currentDeadline`, `checkDeadlines` |
| I7 | **`--keep-failed` is a logical OR** over attached subscribers. | `keepFailedRequested` |
| I8 | **Replay buffer in coordinator memory**, kept as a `headCap` head + `tailCap` tail (default 1 MiB + 3 MiB) with an explicit truncation marker once the tail evicts; replayed frames are tagged `replayed = true`. | `ReplayBuffer` in `build-registry.cc` |
| I9 | **The registry coordinates; it does not run builders.** The session that gets `started = true` (the MISS) drives the build (`log`/`finish`); the registry fans out, manages replay, and owns refcounted lifetime. | `BuildAttachment::started`; coordinator `startBuild` forks the builder |
| I10 | **Lease/epoch fencing seam** — each (re)creation of a key bumps the epoch, so a future persistent/distributed registry can express "who is building K, recover safely if they vanish" without touching callers. | `BuildLease`, `Build::epoch` |
| I11 | **Pluggable auth** — authentication (peer-cred locally, mTLS/identity on a network control plane) is distinct from authorization, which is the same code either way. No call site inlines a `SO_PEERCRED`/same-host assumption. | `BuildAuth` / `BuildAuthPolicy`; socket peer-cred in `peerIsSameUid` |

## 4. Evidence the engineering gates are met

**In-tree functional tests** (`tests/functional/`, gated on the
`build-coordinator` experimental feature, all green in this session):

| Test | Proves |
|---|---|
| `build-dedup-coordinator.sh` | dedup + replay + fan-out: two clients, one real build (same builder-process token), the late joiner receives the pre-attach log via replay (I1, I8, I9) |
| `build-dedup-cancel.sh` | refcounted cancel: the originator is killed, the joiner still completes — the build is not cancelled while another subscriber wants it (I4, I5) |
| `build-dedup-toplevel.sh` | top-level builds dedup via the goal, not only the hook path |
| `build-active-builds.sh` | `queryActive` introspection: the in-flight build is listed while held, empty after completion (I3 filtering, the `QUERY_ACTIVE` op) |

**Prototype gates** (Workstream A, `make check` — the validation plan's record):
`A-dedup`, `A-replay`, `A-backpressure`, `A-sockauth`, `A-crash`, `A-spawn` all
green; `A-throughput` measured within the provisional ceiling (no escalation
needed). `A-sockauth` corresponds in-tree to the peer-credential check at
`accept` (`peerIsSameUid`); `A-backpressure` to the non-blocking per-socket
sink contract (`BuildLogSink` "must not block").

**Operational questions** behind the interface are all resolved as decisions
O1–O7 (deployment model, crash-recovery posture, lazy-spawn lifecycle, replay
cap/UX, `QueryActiveBuilds` privacy, throughput posture, elastic advertisement)
— see the decisions record. None remains open as an F-INT blocker.

## 5. What this sign-off does **not** cover

1. **The public wire.** Build Sessions / log frames / `QueryBuildLog` / extended
   `BuildResult` on the serve & worker protocols are F-WIRE and F-SERVE-DIAG,
   with their own checklists. The client-facing bytes are unchanged by anything
   here.
2. **The deferred serve fields (H3)** — serve `deduplicated` / `builderId`. Their
   *semantics* depend on this Phase 3 design settling; freezing their wire layout
   is forbidden until then.
3. **Cross-user / cross-tenant dedup.** The shipped coordinator runs under a
   single-user experimental gate: `AllowAllAuthPolicy` + a same-uid peer-cred
   socket. The cross-user path — the coordinator recomputing the key from the
   received drv and re-authorizing every subscriber against the resolved key via
   a real `BuildAuthPolicy` — is explicitly deferred (see the `DEFERRED` note in
   `handleStartOrAttach`). The *interface* already expresses it (I3, I11); only
   the policy implementation and the gate-widening are deferred. This is the open
   "CA key-merge + trust" question and is to be decided **with the trust model**,
   not as part of F-INT.
4. **Persistence / re-adoption.** v1 is safe-degrade (decision O2): builders die
   with the coordinator, releasing their `PathLocks`; clients fall back to
   building locally; the existing lock/validity logic guarantees no corruption or
   double-build. A persistent registry that survives a restart is deferred,
   evidence-gated hardening — the `BuildLease` epoch (I10) is the seam that keeps
   it additive.
5. **Sharding / throughput scaling.** v1 is a single-threaded event loop
   (decision O4); per-key sharding is deferred and gated on measurement. Below
   the wire, so a later sharded design is an internal change.

## 6. Residual risks to weigh

- **Shared crash domain in the coordinator.** Builders stay isolated in `fork`ed
  subprocesses (a builder crash is contained exactly as today), but a bug in the
  coordinator's *own* event loop / scheduling code is a shared crash domain
  across the connections it fronts. Mitigation is the one the spike chose
  deliberately: keep the coordinator small. v1's safe-degrade posture (O2) means
  a coordinator crash falls back to local builds, not corruption.
- **The single-user gate must precede widening.** Item 5.3 above is not just a
  TODO: the cross-user re-authorization must land *before* the experimental gate
  is widened to multi-user, or a registry HIT could let one user attach to
  another's build. The gate enforces this today.
- **Throughput is unproven at scale.** The single-threaded loop was measured
  within a provisional ceiling on the prototype, not under production fan-out;
  the sharding seam (key axis) exists but is unexercised.

## 7. Why the stakes of this freeze are bounded

F-INT freezes an **internal** interface. Unlike a wire freeze (§7 of the RFC,
which "cannot be walked back"), nothing external — no client, no Hydra, no
on-disk format — depends on the `BuildRegistry` operations or the control-socket
records. The whole point of the coordinator design was to leave the
client-facing wire byte-identical so the mechanism could change independently.
So a later revision of these operations is an ordinary internal refactor, not a
compatibility event. The sign-off being requested is therefore "this interface
is sound enough to build Phase 3 implementation on," not an irrevocable
commitment — which is exactly why the engineering evidence in §4, not a
multi-release soak, is the appropriate bar for it.
