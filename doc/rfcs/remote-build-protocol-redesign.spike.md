# Spike: Cross-process build coordination for dedup/attach/replay

| | |
|------------------|------------------------------------------------|
| **Status**       | Spike / design + prototype plan                |
| **Gates**        | RFC §8 "Spike" milestone → Phase 3 wire freeze |
| **Parent**       | [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md) |
| **Resolves**     | RFC Q0 (mechanism), Q1 (replay-buffer location), and contributes to Q3 (CA resolution timing) |

> This is a **design spike**, not an implementation. Its single job is to let
> the team commit to **one** cross-process coordination mechanism *before* any
> Phase 3 wire surface is frozen (RFC §8, §10 Q0). It contains a recommendation,
> the concrete interface of the recommended mechanism, and a throwaway prototype
> plan with explicit success criteria. It deliberately writes **no production
> C++**; the sketches below are illustrative pseudocode for an experimental
> branch that is expected to be thrown away once the design is validated.

---

## 0. The question this spike answers

The RFC's headline feature — **one build on a builder, many clients attached to
its live log, with replay for late joiners and reference-counted cancellation**
(G3, §4.3) — is blocked on a single process-model fact the RFC now states
plainly (RFC §2.3): **the stock `nix-daemon` is fork-per-connection.** There is no shared `Worker`, goal map, or
log buffer that two connections can both see, so the "share one build, fan out
its log" mechanism does **not** exist even latently and cannot be "lifted" from
existing code.

RFC §4.3.3 lists three candidate mechanisms and explicitly declines to pick one,
deferring to this spike:

1. **Coordinator process** — a long-lived per-store process owns the registry,
   replay buffers, and refcounts; connection children become thin relays.
2. **Shared memory + published log ring buffer** keyed on the resolved drv,
   building child as writer, attaching children as readers.
3. **Re-architect the daemon to single-process multiplexed**, abandoning
   fork-per-connection.

This document verifies the load-bearing facts (§1), evaluates the three
mechanisms against explicit criteria (§2), specifies the chosen mechanism's
interface (§3), defines a prototype and its pass/fail bar (§4), and confirms the
RFC's invariants are preserved (§5). It ends with a one-paragraph recommendation
and the decisions still owed to a human (§6).

---

## 1. Load-bearing facts, verified against the tree

Every claim the design rests on was re-verified against the current working tree
(not taken from the RFC). Citations are `file:line`.

### 1.1 The daemon forks one child process per connection

`daemonLoop` (`src/nix/unix/daemon.cc:247`) accepts a connection and then:

```cpp
// src/nix/unix/daemon.cc:352-382  (elided)
// Fork a child to handle the connection.
startProcess(
    [&, storeConfig, closeListeners = std::move(closeListeners)]() {
        closeListeners();
        if (setsid() == -1) throw SysError("creating a new session");
        ...
        // Handle the connection.
        auto store = storeConfig->openStore();   // :376  -- its OWN store
        store->init();                            // :377
        processConnection(store, FdSource(remote.get()), FdSink(remote.get()),
                          trusted, NotRecursive); // :378
        exit(0);                                  // :380
    },
    options);
```

So **each connection is a separate `fork()`ed process** with its **own
`openStore()`** (`:376`) and its own `processConnection`
(`src/libstore/daemon.cc:1022`). Nothing in the parent is shared with the child
except inherited file descriptors at fork time. This is the fact that makes the
whole spike necessary; it is verified, not assumed.

### 1.2 The `Worker` and its goal-dedup live entirely inside one process

`processConnection` builds a per-process `Worker` that owns the goal maps:

* `Worker::initGoalIfNeeded` (`src/libstore/build/worker.cc:59`) is the in-process
  dedup primitive: it returns an existing live goal from a weak-pointer map or
  creates one.
* The map itself: `derivationGoals` is
  `std::map<StorePath, std::map<OutputName, std::weak_ptr<DerivationGoal>>>`
  (`src/libstore/include/nix/store/build/worker.hh:123`); `makeDerivationGoal`
  keys into it at `worker.cc:89-97`.
* The `Worker` holds **both** stores — `Store & store` (build, `worker.hh:200`)
  and `Store & evalStore` (`worker.hh:201`, ctor `worker.hh:236`). This is the
  in-process embodiment of the eval-store/build-store split (RFC §2.4), and it
  matters for §2.7 below.

This map is per-`Worker`, per-`processConnection`, per-forked-child. **Two
connections cannot see each other's goals.** `initGoalIfNeeded` dedups one
client asking twice in one invocation; it does nothing across connections.

### 1.3 What actually coalesces cross-connection builds today: output `PathLocks`

`DerivationBuildingGoal` acquires filesystem locks on its output paths before
building, in the `acquireResources` coroutine
(`src/libstore/build/derivation-building-goal.cc:437`):

```cpp
// :470
if (!outputLocks.lockPaths(lockFiles, "", false)) { ... }   // non-blocking try
...
// :482  blocking retry loop
} while (!outputLocks.lockPaths(lockFiles, "", false));
...
// :496-497
outputLocks.setDeletion(true);
outputLocks.unlock();
```

A second process building the same input-addressed derivation **blocks** on the
same `.lock` file; when it wakes, the outputs are already valid and it skips the
rebuild. This serializes redundant *work* but provides **no log fan-out, no
attach, no replay** — exactly as RFC §2.3 states. It is the floor we are
building above, and it keeps working unchanged for clients/peers that never
negotiate the new capability.

### 1.4 How a goal's log reaches the client today: `TunnelLogger` → `STDERR_*`

Inside the build child, the ambient `logger` is a `TunnelLogger`
(`src/libstore/daemon.cc:50`). The `Worker`/goal writes activities and log lines
to that ambient logger, and the `TunnelLogger`:

* serializes each line as `STDERR_NEXT` and enqueues it
  (`daemon.cc:95-96`, `enqueueMsg` at `:70`);
* brackets operations with `startWork()` (`:114`) / `stopWork()` (`:129`),
  the latter terminating the stream with `STDERR_LAST` (`:136`);
* emits `STDERR_START_ACTIVITY` / `STDERR_STOP_ACTIVITY` / `STDERR_RESULT`
  frames for structured activities.

On the client, `RemoteStore::ConnectionHandle::processStderr`
(`src/libstore/remote-store.cc:166`) consumes those frames and re-injects them
into the client's own `Logger`; `STDERR_START_ACTIVITY` carries a `parent`
`ActivityId` (`src/libstore/worker-protocol-connection.cc:78-85`), which is the
hook the RFC's per-build activity tagging (§4.2) relies on.

**The crucial structural point for this spike:** the `TunnelLogger` is bound to
**one socket** — the connected client's. It is a per-process, per-connection
sink. "Fan out one build's log to N clients" means "deliver these `STDERR_*`
frames to N different `TunnelLogger`s living in N different processes," which the
current architecture has no way to express.

### 1.5 How disconnect cancels a build today: `MonitorFdHup` → `triggerInterrupt`

The serve build paths install a `MonitorFdHup` on the client fd
(`src/nix/nix-store/nix-store.cc:1009` for `BuildPaths`, `:1032` for
`BuildDerivation`; `opServe` at `:878`, `getBuildSettings` lambda suppressing
logs at `:908-909`). The worker-protocol daemon path does the equivalent.

`MonitorFdHup` (`src/libutil/unix/include/nix/util/monitor-fd.hh:20`) runs a
thread that `poll()`s the fd and, on `POLLHUP` (client gone), calls
`unix::triggerInterrupt()` (`monitor-fd.hh:120`, Linux; `:93`, Apple). That sets
the process-wide interrupt flag, the in-flight goal throws out, and the child
`exit(0)`s.

So today **cancellation is 1:1 with the connection**: one disconnect kills the
one build that child was running. There is no subscriber count, so "don't cancel
while another client is still attached" is *unrepresentable* in the stock daemon.
This is precisely the semantics the RFC needs to invert (§4.3, refcounted
cancel), and it is why that semantics cannot be retrofitted without a component
that outlives the individual connection.

### 1.6 The `getBuildLogExact` gap (context for where replay hands off)

`SSHStore::getBuildLogExact` still `unsupported(...)`s with the standing
`// FIXME extend daemon protocol` (`src/libstore/ssh-store.cc:64-67`). This is
Phase 0 / Gap A and is **out of scope** for this spike, but it matters here for
one reason: it defines the *handoff boundary* of replay. Live replay (§3.5)
covers a late joiner who attaches **while the build is running**; once the build
is finished the in-memory buffer is gone and the source of truth is the
persisted log fetched via the Phase 0 `QueryBuildLog` path. The spike must not
reinvent post-hoc fetch; it must cleanly hand off to it.

### 1.7 Fact-check summary

| Fact the design rests on | Citation | Verified |
|---|---|---|
| Daemon forks a child per connection | `src/nix/unix/daemon.cc:352-382` | ✅ |
| Each child opens its own store | `src/nix/unix/daemon.cc:376` | ✅ |
| `processConnection` is per-child | `src/libstore/daemon.cc:1022` | ✅ |
| In-process dedup = `initGoalIfNeeded` | `src/libstore/build/worker.cc:59` | ✅ |
| Goal map is per-`Worker` | `worker.hh:123`; `worker.cc:89-97` | ✅ |
| `Worker` holds build + eval store | `worker.hh:201,236` | ✅ |
| Cross-conn coalescing = output `PathLocks` | `derivation-building-goal.cc:437,470,482` | ✅ |
| Log → client via `TunnelLogger`/`STDERR_*` | `daemon.cc:50,70,95-96,114,129,136` | ✅ |
| Client consumes via `processStderr`, `parent` on activities | `remote-store.cc:166`; `worker-protocol-connection.cc:78-85` | ✅ |
| Disconnect → `triggerInterrupt` → child exits | `monitor-fd.hh:20,120`; `nix-store.cc:1009,1032` | ✅ |
| `getBuildLogExact` unsupported over ssh-ng | `ssh-store.cc:64-67` | ✅ |

---

## 2. Mechanism evaluation

### 2.1 Criteria

The criteria that must be **confronted, not hand-waved** are **fault
isolation, crash cleanup, and cross-tenant log leakage**. The criteria below
are weighted accordingly.

1. **Fault isolation.** Today a crashing/OOM-killed build is contained to one
   forked child; the daemon and other connections survive. What is the blast
   radius *after* the mechanism is in place?
2. **Lifecycle & cleanup on crash/restart.** If the build process, the
   coordinating component, or a connection child dies, what state leaks
   (registry entries, locks, buffers, fds), and who reaps it?
3. **Backpressure.** A fast build can emit log faster than a slow attached
   client drains it. Where does the unread data accumulate, and what stops it
   from (a) wedging the build, or (b) growing without bound?
4. **Security / trust (RFC §6).** The coordinating component now spans clients of
   potentially different trust levels. How is per-session authorization enforced
   so a client cannot read (or even enumerate) a build it could not itself have
   requested? A shared log path is a cross-tenant exfiltration oracle if this is
   wrong.
5. **Implementation blast radius.** How much *new* surface, how much *changed*
   surface in code that currently has strong invariants (the trust handler, the
   fork loop), and how throw-away-able is the prototype?
6. **Interaction with the eval-store/build-store split (RFC §2.4, §4.6).** The
   `Worker` carries `evalStore` (`worker.hh:201`); whoever runs the build must be
   able to copy `.drv` closures eval→build and outputs back. Does the mechanism
   keep that working without moving the split into a new process boundary?
7. **What it lets the wire promise.** RFC §10 Q0 says the mechanism *bounds*
   what `QueryActiveBuilds` (§4.3.2) and session re-attach (§4.7.4) can offer.
   Does it support builds that outlive their originating connection?

### 2.2 Mechanism 1 — Coordinator process

A long-lived, per-store coordinator (a new role of `nix-daemon`, or a sidecar
launched on demand and reaped when idle) owns the Build Registry, the replay
buffers, and the subscriber refcounts, and **runs the actual `Worker`**. The
forked connection children become thin: on a build request a child does a
*start-or-attach* RPC to the coordinator over a Unix socket, then relays the
coordinator's `STDERR_*` frames down its own client socket and forwards
disconnect/cancel.

| Criterion | Assessment |
|---|---|
| **Fault isolation** | **Changed, and this is the central honest trade-off — M1 is a *hybrid*, isolated on one axis and not the other.** The coordinator does **not** run builds inline: it keeps the existing `fork()` per build (the daemon already `fork()`s per build internally via `startProcess`/build hooks), so a **builder-subprocess** crash/OOM (the sandboxed, untrusted code) kills only that *build* subprocess; the coordinator observes `SIGCHLD`/exit, marks the `Build` failed, and fans the failure out to subscribers. On that axis isolation **equals today's** (builder crashes stay contained). **But** the coordinator runs the real `Worker` — goal/resolution/scheduling/fan-out logic — for **all** clients in one address space, so a bug in *that* (non-sandboxed) code crashes the coordinator and takes down **every** attached client's build at once. **On the "Worker-logic crash" axis, M1 ≈ M3** (§2.4): both are a single shared crash domain for the scheduling layer; M1's only isolation advantage over M3 is that *builder* code stays forked. So the honest statement is: M1 preserves today's **builder-crash** containment but introduces a shared crash domain for coordinator-`Worker` bugs across connections (the same class M3 carries). The mitigations are: keep the coordinator small and supervisory (it links no builder code into its heap), and the next row's degrade-to-`PathLocks` safety net bounds the *consequences* of a coordinator crash even though it cannot prevent the shared-fate. |
| **Lifecycle & cleanup** | The coordinator is the one place to do cleanup, which is the upside. Note the builder subprocess's **parent moves from the connection child to the coordinator** (it is the coordinator that now `fork()`s it), so `SIGCHLD`/`wait()` reaping ownership moves with it — the coordinator, not the child, must install the `SIGCHLD` handling and reap. On build-subprocess death: reap, fail the `Build`, release its output `PathLocks` (still the cross-process correctness floor, §1.3), drop the registry entry, flush the persisted log. On connection-child death: the coordinator sees the per-session control socket close and decrements the refcount (§3.4). **On coordinator death:** every registry entry and buffer is lost; in-flight builds are orphaned. We make this *safe* (not lossless): builds hold real output `PathLocks` on disk, so a restarted coordinator + the existing lock/validity logic never double-builds or corrupts — the worst case degrades to **today's** behaviour (work coalesced by locks, no fan-out) until the build finishes. The coordinator is restartable; children that lose it fall back to building locally (see §5.1). Socket is a well-known path under the store's state dir; a stale socket is detected and re-created on daemon start. |
| **Backpressure** | Cleanest of the three. Each subscriber is a separate socket with its own kernel send buffer. The coordinator writes each frame to each subscriber's pipe; a slow subscriber's pipe fills. Policy (decided here, §3.6): the coordinator never blocks the build on a slow subscriber — it buffers up to a per-session cap, and past the cap it **drops that one session to "replay-from-persisted-log" mode** (disconnect its live tail, leave the build and other subscribers untouched). The build's own rate is bounded by the replay-buffer cap (§3.5), not by the slowest client. |
| **Security / trust** | Strongest story. The coordinator authenticates each connecting child the same way `daemonLoop` already does (`authPeer`, `src/nix/unix/daemon.cc:342`); the **child passes the session's resolved trust level and the derivation it is authorized to build** in the start-or-attach RPC, and the coordinator checks authorization **before** subscribing (RFC §6). Because the coordinator is a normal Nix process with the existing trust machinery, the check is the *same* code path as today's `BuildDerivation` handler, not a new ACL system. `QueryActiveBuilds` filtering (§3.7) lives in one place. |
| **Impl. blast radius** | Medium. New: a coordinator main loop, a small control protocol (start-or-attach / subscribe / cancel / query), and frame relay in the connection child. Changed: the daemon gains a "connect to coordinator if present, else build locally" branch. Crucially **the client-facing wire (worker/serve protocol) does not change shape** — the child still speaks `STDERR_*` to the client. The prototype is genuinely throw-away: it can be a standalone sidecar binary + a feature-flagged relay path, deleted wholesale if rejected. |
| **Eval/build split** | Clean. The coordinator runs a `Worker` with both `store` and `evalStore` (`worker.hh:236`) exactly as `processConnection` does today. The drv-closure copy (`copyDrvsFromEvalStore`, `remote-store.cc:548-566`) happens client-side **before** the request reaches the builder (RFC §2.4), so by the time start-or-attach runs, the `.drv` closure is already on the build store. The coordinator never needs the client's eval store. **One subtlety (feeds back into the RFC, §5.4):** the build key is the *resolved* drv, and resolution may need eval-store-only `.drv` inputs — but resolution is done client-side too (the client resolves before asking, or the build store already has the inputs). The coordinator keys on what it receives. |
| **Wire promise** | Supports builds outliving their connection (the coordinator owns the build), so `QueryActiveBuilds` and re-attach (§4.7.4) are *possible* — bounded only by coordinator uptime. This is the only mechanism that makes re-attach truthful on the stock daemon. |

### 2.3 Mechanism 2 — Shared memory + published log ring buffer

A POSIX shared-memory segment (per resolved-drv-key) holds a lock-free ring
buffer of log frames; the building child is the writer, attaching children
`mmap` it as readers. A small shared registry/refcount table (also in shared
memory, under a mutex/futex) maps build key → segment + state.

| Criterion | Assessment |
|---|---|
| **Fault isolation** | **Worst.** The whole point of shared memory is a shared failure domain. A builder that corrupts the ring (wild write, mismatched producer/consumer indices after a crash mid-write) can wedge or mislead *every* reader. A reader crash is benign; a **writer crash mid-frame** leaves the ring in an ambiguous state with no transactional recovery — robust lock-free SPMC ring design across a trust boundary is notoriously hard to get right (RFC §4.3.3 mechanism 2 warns this "pushes lifecycle, cleanup-on-crash, and backpressure into shared-memory bookkeeping that is easy to get wrong"). |
| **Lifecycle & cleanup** | Hardest. Shared segments and the registry table outlive any single process by design, so a crashed writer leaves an orphaned segment and a registry entry with a now-bogus writer pid and a non-zero refcount. Reaping requires a liveness protocol (writer pid + a watchdog, or `pid_t` liveness checks racing against pid reuse). `shm_unlink` ownership is ambiguous when readers may still be attached. Rob-futex/`EOWNERDEAD` handling is required to recover the registry mutex if a holder dies in the critical section. Every one of these is a classic shared-memory footgun. |
| **Backpressure** | Awkward. A ring buffer is fixed-size: either the writer overwrites unread data (a slow reader silently loses log it can never recover except via post-hoc fetch — acceptable but must be explicit), or the writer blocks when the ring is full (a slow reader now throttles the **build itself** — unacceptable, the build's speed must not depend on a client). The overwrite policy is the only sane one, which means the ring is really just an in-shm replay buffer with lossy live tail — at which point most of the value is the registry, not the ring. |
| **Security / trust** | Dangerous. The segment spans clients. Access control is filesystem permissions on the shm object plus discipline; there is no per-frame authorization. If two tenants of different trust map the same key's segment, the segment **is** the leak. You can scope segments per (key, authorized-uid-set), but now you are maintaining an ACL in shared memory — the exact thing a coordinator does in normal memory with the existing trust code. The trust check cannot be "before subscribe" in any clean sense because subscribe *is* an `mmap`. |
| **Impl. blast radius** | Deceptively large. "No new daemon" is the headline appeal, but the correctness surface (lock-free ring, robust mutex, liveness/reaping, segment ACLs) is *more* total complexity than a coordinator, concentrated in the hardest-to-test category of bug (concurrency + crash recovery across processes). Hard to make genuinely throw-away because a half-built shm protocol is hard to reason about even as a prototype. |
| **Eval/build split** | Neutral-to-awkward. The building child still has the `Worker` with both stores, so the split itself is fine — but a late joiner attaching via shm never re-runs the build, so it must obtain the `BuildResult` (and trigger its own output copy-back if it has a *different* build store, which in the daemon case it does not) through the registry table, adding more shared structure. |
| **Wire promise** | A build can outlive a *connection* (the segment persists), but only as long as the **writer** lives — and the writer is a forked connection child that dies on its own client's disconnect (§1.5) unless decoupled. To make builds truly connection-independent you end up electing a long-lived writer… which is a coordinator. So this mechanism tends to *converge toward* mechanism 1 under the weight of its own requirements. |

### 2.4 Mechanism 3 — Single-process multiplexed daemon

Abandon fork-per-connection: one daemon process with an event loop multiplexes
all connections, so §4.3's in-process registry/broadcaster/refcount become
*literally* the in-memory data structures the RFC originally (wrongly) assumed
already existed.

| Criterion | Assessment |
|---|---|
| **Fault isolation** | **Strictly worse than today, and unavoidably so.** This is the trade-off the RFC itself names (§4.3.3.3: "one crashing build can no longer be contained to a child"). Builds already `fork()` for the actual builder, so a builder *process* crash is still contained — but all the **scheduling, goal state, logging, and connection handling** for every client now share one heap and one crash. A bug in goal/Worker code (not the sandboxed builder) takes down the entire daemon and every connected client at once. Today that same bug takes down one connection. For a piece of infrastructure whose entire value proposition is reliability, this is the hardest sell. |
| **Lifecycle & cleanup** | In-process cleanup is *easy* (RAII, one address space) — but the flip side of "one crash kills everything" is that recovery is all-or-nothing: the daemon restarts and every client reconnects. No partial degradation. |
| **Backpressure** | In-process fan-out is the easiest to write (write to N in-memory `Logger`s / socket buffers), with the same slow-client policy as mechanism 1, but now a slow client's buffer growth competes for the **daemon's** heap with everything else. |
| **Security / trust** | Per-session checks are easy (same heap, existing trust code), comparable to mechanism 1. No shared-memory ACL problem. |
| **Impl. blast radius** | **Largest by far.** This is a ground-up rewrite of `daemonLoop` and `processConnection` from blocking-per-process to async multiplexed, touching the most invariant-heavy, security-sensitive code in the daemon (`daemon.cc`, the trust handler, signal handling, `MonitorFdHup` semantics). Not throw-away in any sense — you cannot prototype "the daemon is now single-process" behind a flag. It is a multi-quarter project with its own RFC. |
| **Eval/build split** | Fine in principle (one `Worker` per store, shared), but reworking how per-connection `evalStore`s map onto shared `Worker`s is extra design. |
| **Wire promise** | Best in the abstract — builds trivially outlive connections, re-attach and `QueryActiveBuilds` are free — but you pay for it with the isolation and blast-radius rows above. |

### 2.5 Scoring and recommendation

| Criterion (weight) | M1 Coordinator | M2 Shared memory | M3 Single-process |
|---|---|---|---|
| Fault isolation | ◑ builder crashes contained; coordinator-`Worker` bug shared (≈ M3 on that axis) | ● worst (shared failure domain, incl. corruption) | ● strictly worse than today |
| Crash cleanup | ◔ one place; coordinator-death degrades to today | ● orphan segments, robust-futex, pid reuse | ◑ all-or-nothing |
| Backpressure | ○ per-socket, drop-slow-client | ◑ lossy ring or throttles build | ◔ easy but shares daemon heap |
| Security / trust | ○ existing trust code, check-before-subscribe | ● segment *is* the leak | ○ existing trust code |
| Impl. blast radius | ◑ medium, throw-away prototype | ◑ deceptively large, hard to prototype | ● largest, not throw-away |
| Eval/build split | ○ unchanged | ◑ extra registry plumbing | ◑ extra design |
| Wire promise (outlive conn) | ○ yes (coordinator uptime-bounded) | ◔ only converging toward M1 | ○ yes |

(○ good · ◔ ok · ◑ mixed · ● poor)

**Recommendation: Mechanism 1 — the coordinator process — as the RFC already
suspected (§4.3.3).** It is the only option that (a) **preserves today's
*builder-crash* isolation** by keeping builders in `fork()`ed subprocesses while
adding a *small, supervisory* coordinator — with the honest caveat (§2.2) that a
bug in the coordinator's own `Worker`/scheduling logic is a shared crash domain
across connections, the same class M3 carries; M1's win over M3 here is that it
is a far smaller, more contained piece of code, not that the crash domain
differs in kind, (b) concentrates the genuinely hard parts — refcounted
cancellation, replay-buffer ownership, `QueryActiveBuilds`, trust filtering — in
**one place running the existing trust code**, (c) keeps the client-facing wire
identical so Phase 3 framing is decoupled from the mechanism, and (d) yields a
**throw-away prototype** (a sidecar + a flagged relay path). Mechanism 2 trades
the coordinator for a pile of shared-memory crash-recovery footguns that
*reintroduce* a coordinator under another name, with a worse trust story.
Mechanism 3 is the cleanest end-state on paper but the worst on the axis the
reviewer cares most about (isolation) and is far too large to be a spike — if it
ever happens it is its own RFC, not a prerequisite for Phase 3.

---

## 3. The coordinator interface (concrete)

This section is the spike's contract: enough interface to freeze the Phase 3
client-facing wire *independently*, and enough internal shape to prototype.

### 3.1 Topology

```
client A ──socket──▶ daemon child A ─┐
                                     ├─control socket──▶  coordinator
client B ──socket──▶ daemon child B ─┘                    (per store, long-lived)
                                                              │
                                                  Build Registry: key → Build
                                                  Build = { Worker-run build subproc,
                                                            replay buffer, subscriber set,
                                                            refcount, persisted-log writer }
```

* **One coordinator per store directory**, addressed by a Unix socket at a
  well-known path under the store's state dir (e.g.
  `$NIX_STATE_DIR/coordinator.socket`), **permissioned and peer-verified per
  §3.7.1** so only genuine daemon children of the right uid can speak to it.
  Started lazily by the first daemon child that wants it (or as a daemon
  sub-role), idle-exits after a grace period with no builds and no subscribers.
  **Lazy spawn has two lifecycle races that the prototype must handle (open
  question, §6):** (a) two children racing to spawn the coordinator — resolved
  by an atomic `bind()`/lock-file election where the loser connects to the
  winner; and (b) the coordinator idle-exiting between a child's `connect()` and
  its first request — resolved by a "decline-and-respawn" handshake (a child that
  hits a closing/closed coordinator re-elects and retries). These must be called
  out so a flaky spawn is not misdiagnosed as a coordination bug.
* **Connection children stay fork-per-connection** (§1.1 unchanged). They gain
  one new behaviour: for a build request that negotiated the new capability,
  instead of running the build in-process they open a **control connection** to
  the coordinator and relay.
* The coordinator runs the real `Worker` (build + eval store) and `fork()`s the
  builder per build exactly as the daemon does today — **isolation preserved**
  (§2.2).

### 3.2 The control protocol (child ↔ coordinator)

A small, internal, **unversioned-with-the-public-wire** protocol (it is an
implementation detail of one machine; it is *not* the worker/serve protocol and
never reaches a client). Illustrative message set:

```
// child → coordinator
START_OR_ATTACH { buildKey, drvForBuild, sessionAuth, replayWanted }   → SubscriptionId
SUBSCRIBE      { SubscriptionId }                                       // begins frame stream
UNSUBSCRIBE    { SubscriptionId }                                       // detach, decrement refcount
CANCEL_HINT    { SubscriptionId }                                       // "my client wants to stop"
QUERY_ACTIVE   { sessionAuth }                                          → [BuildStatus...]

// coordinator → child   (on a SubscriptionId's stream)
FRAME          { STDERR_* bytes, replayed: bool }     // relayed verbatim to the client socket
BUILD_RESULT   { BuildResult + {deduplicated, builderId, logRef} }
ATTACH_STATE   { resolving | building | finished }    // for Q3, see §3.8
```

* **`buildKey`** = the resolved derivation (RFC §4.3 "build key"): the derivation
  with every `inputDrv` replaced by its concrete output path. Input-addressed
  drvs map 1:1; CA drvs that resolve identically coalesce. The child computes (or
  forwards) the resolved drv; the coordinator keys the registry on it.
* **`sessionAuth`** = the client's **authenticated identity** as the child
  established it via `authPeer` (`daemon.cc:342`, def `:212`) — uid and
  trusted/untrusted flag. The coordinator trusts this *identity* only after
  peer-verifying that the child itself is a genuine daemon process (§3.7.1), but
  **re-derives the authorization decision** (may this identity build/attach
  `drvForBuild`) itself before subscribing — it does not trust the child to have
  authorized, only to have authenticated. See §3.7.

### 3.3 `START_OR_ATTACH` — the core operation

```
coordinator.handle(START_OR_ATTACH { buildKey, drvForBuild, sessionAuth, replayWanted }):
    authorize(sessionAuth, buildKey)            # §3.7 — BEFORE touching the registry
    build = registry.get(buildKey)
    if build is None:                            # MISS
        build = Build(buildKey)
        build.persistedLog = openLogWriter(buildKey)     # §4.4 addBuildLog path
        build.replay = ReplayBuffer(cap)                 # §3.5
        build.startWorkerBuild(drvForBuild)              # fork()s the builder; Worker holds eval+build store
        registry.put(buildKey, build)
        sub = build.subscribe(replayWanted=False); sub.deduplicated = False
    else:                                        # HIT
        sub = build.subscribe(replayWanted)
        sub.deduplicated = True
    build.refcount += 1
    return sub.id
```

Two clients racing the same key: the registry is single-threaded in the
coordinator (one event loop), so exactly one `START_OR_ATTACH` sees the miss and
creates the `Build`; the second sees the hit. **Exactly one build runs** — this
is what the prototype must prove (§4).

### 3.4 Subscribe / unsubscribe / refcounted cancellation across processes

This is the part that is *unrepresentable* in the stock daemon (§1.5) and the
reason a coordinator is needed at all.

* **Subscribe** = the child issues `SUBSCRIBE`; the coordinator adds the child's
  stream fd to `build.subscribers`. If `replayWanted`, it first drains the replay
  buffer (frames tagged `replayed=true`, §3.5) then joins the live tail.
* **The child relays frames** straight down its own client socket. A frame the
  coordinator emits as `FRAME{STDERR_NEXT ...}` is written by the child to its
  `FdSink(remote)` exactly as a local `TunnelLogger` would (§1.4) — the client
  cannot tell the difference, so the **public wire is unchanged**.
* **Unsubscribe / disconnect.** The child still installs `MonitorFdHup` on its
  *client* fd (§1.5), but its action changes: instead of `triggerInterrupt`
  killing a local build, the HUP causes the child to send `UNSUBSCRIBE`
  (`CANCEL_HINT` if the client actively cancelled) and exit. The coordinator
  decrements `build.refcount`.
* **Refcounted cancellation (the inverted semantics).**

  ```
  coordinator.on(UNSUBSCRIBE sub):
      build = sub.build
      build.subscribers.remove(sub.streamFd)
      build.refcount -= 1
      if build.refcount == 0 and not build.hasRootReasonToContinue():   # §5.2: --keep-going etc.
          build.cancel()        # interrupt the builder subprocess, release PathLocks, drop entry
      # else: build continues for the remaining subscribers
  ```

  A late joiner attaching between "last subscriber left" and actual cancel
  re-increments the refcount and the build survives (single-threaded registry
  makes this race-free). This is the exact semantics RFC §4.3 promises and §10
  Q2 wants a test matrix for.

### 3.5 Replay buffer — location resolved (RFC Q1)

**The replay buffer lives in the coordinator's own process memory, per
`Build`.** This is the spike's answer to RFC Q1, which §4.3.1 says cannot be
decided independently of the mechanism:

* It is plain in-process memory (not shared memory, not the connection child's
  memory), because the coordinator is the one process that outlives all
  connections and sees the build from the start.
* **Policy:** retain the full structured log up to a configurable cap
  (default proposal: a byte cap, e.g. 4 MiB of frames); past the cap, switch to
  **head + tail with an explicit `…truncated N frames…` marker** so very long
  builds (kernel, LLVM) do not grow coordinator memory without bound. The head
  preserves the configure/early-failure context; the tail preserves the live
  edge a late joiner is about to follow.
* On attach with `replayWanted`, frames are streamed `replayed=true` so the
  client renders them dimmed/collapsed (RFC §4.3.1), then the session transitions
  to the live tail. The handoff is a single atomic "snapshot buffer + register as
  live subscriber" step under the event loop, so no frame is dropped or
  duplicated at the seam.
* **After the build finishes**, the buffer is discarded; any further joiner gets
  the whole log via the persisted-log fetch path (Phase 0 `QueryBuildLog`,
  §1.6) — the buffer is strictly a *live-build* structure.

Because the buffer is coordinator memory, its size policy (Q1) and the mechanism
(Q0) are now decided **together**, as the RFC required.

### 3.6 Backpressure policy (resolved)

* The build writes frames to the coordinator (its own subprocess → coordinator
  pipe) at builder speed; the coordinator appends to (a) the replay buffer
  (bounded, §3.5), (b) the persisted-log writer, and (c) each subscriber's
  stream fd.
* **The build is never throttled by a slow subscriber.** Each subscriber fd is
  non-blocking with a per-session outbound cap. If a subscriber's buffer exceeds
  the cap (slow client, §criterion 3), the coordinator **demotes that one
  session**: it stops feeding it the live tail, sends a marker, and lets the
  client fall back to fetching the persisted log after the fact. The build and
  every other subscriber are unaffected.
* The build's own memory is bounded by the replay cap, not by the number or
  speed of subscribers.

### 3.7 Trust enforcement (RFC §6) — socket authentication, then authorization before subscribe

The coordinator spans clients of different trust levels, so this is the
security linchpin of the whole mechanism. It has **two** distinct layers that
the spike must keep separate; conflating them is how a shared coordinator turns
into the very cross-tenant exfiltration oracle M1 is supposed to *avoid* over M2
(§2.3).

#### 3.7.1 Authenticating the control socket itself (the linchpin)

The per-subscribe authorization check below is worthless if **any** local
process can open the control socket and assert a `sessionAuth`. So the control
socket is secured exactly as the daemon socket is, and the coordinator verifies
its peer:

* **Socket access control.** The control socket
  (`$NIX_STATE_DIR/coordinator.socket`) is created with restrictive ownership
  and mode — owned by the daemon's uid/`nix-daemon` group, mode `0660` — so only
  legitimate daemon children (running as the same uid, the only processes that
  ever `fork()` from `daemonLoop`) can connect at all. This mirrors how the
  daemon's own listening socket is permissioned.
* **Peer-credential check.** On every accepted control connection the
  coordinator reads the connecting peer's `SO_PEERCRED` (uid/pid) — the same
  primitive the daemon already uses on its own socket: `getPeerInfo`
  (defined `src/libcmd/unix/unix-socket-server.cc:26`, using `SO_PEERCRED`
  on Linux / `LOCAL_PEERCRED` on macOS/BSD) wrapped by `authPeer`, called as
  `unix::getPeerInfo` at `daemon.cc:341` (`authPeer` def `:212`) — and
  **requires the peer to be a daemon process running as the daemon's own
  uid.** A connection from any other uid is refused before a single byte of
  `sessionAuth` is read. An attacker who cannot already run code as the
  daemon uid cannot forge `sessionAuth`.

This is the single most important property to get right, and it is what makes
"the coordinator trusts the child's reported identity" safe: the coordinator
trusts the child *because it has cryptographically/kernel-verified that the peer
is a real daemon child of the right uid*, not because it takes the child's word.

#### 3.7.2 Authentication vs. authorization — what the coordinator trusts the child for

These are deliberately split:

* **Authentication (delegated to the child, then peer-verified).** Only the
  connection child holds the client peer's `SO_PEERCRED`/`authPeer` result
  (`daemon.cc:342`, def `:212`) — the coordinator never sees the *client's*
  socket. So the coordinator **does** trust the child's *reported authenticated
  identity* (uid, trusted/untrusted flag), having first established via §3.7.1
  that the child is a genuine daemon process. It is not "never trust the child";
  it is "trust the authenticated identity a verified daemon child reports."
* **Authorization (re-derived by the coordinator, not delegated).** Whether that
  identity may build/attach **this** derivation is decided by the coordinator
  itself, **before** adding the session to `build.subscribers`, by running the
  **same authorization check the `BuildDerivation` handler already enforces**
  (RFC §6, the `daemon.cc` trust comment referenced from
  `build-remote.cc:324-329`). The coordinator does not trust the child to have
  made this decision; it re-derives it from the reported identity + the
  derivation. An untrusted identity may attach only to a build whose derivation
  it is **itself authorized to build** (CA derivations, or where the builder
  trusts it). It can never attach by guessing a `buildKey` for a build it could
  not have requested — so dedup is **not** a cross-tenant log oracle.

#### 3.7.3 `QueryActiveBuilds` filtering

`QUERY_ACTIVE` is filtered by the same authorization layer (RFC §4.3.2, §6,
§10 Q5): an untrusted caller sees only builds it could itself have requested, or
an aggregate count — never another tenant's derivation names. Single chokepoint,
because all queries go through the coordinator.

The key property: **authentication is kernel-verified peer creds on a
permissioned socket; authorization is the existing per-session trust code, run
in one process, before subscription** — not a new ACL system bolted onto a
shared artifact (contrast M2, §2.3), and not a socket any local process can
speak to.

### 3.8 CA resolution timing (RFC Q3) — registry exposes a `resolving` pre-state

RFC §10 Q3 asks: does the registry key on the resolved drv only, or also expose a
"resolving" pre-state late joiners can attach to? For deep CA graphs the
resolution itself can race, so two clients resolving the same eventual build can
arrive *before* either has a resolved key.

**Decision for the spike: key primarily on the resolved drv, but admit a
short-lived `resolving` pre-registration to close the race.**

* When a client begins resolving a CA derivation, the coordinator may register a
  **provisional entry keyed on the unresolved drv path** in state `resolving`
  (`ATTACH_STATE{resolving}`). A second client resolving the *same unresolved
  drv* attaches to the provisional entry instead of starting its own resolution.
* When resolution completes, the provisional entry is **promoted/merged** into
  the real entry keyed on the resolved drv. If two distinct unresolved drvs
  resolve to the **same** resolved key, their provisional entries merge into the
  one real `Build` at promotion time (this is exactly the CA-coalescing the
  resolved key is meant to capture).
* If resolution itself fails, the provisional entry fails and fans the failure
  out to its (few) attached resolvers.
* Subscribers attached during `resolving` simply see resolution activity first,
  then the build's live tail — one continuous stream.

This is a **pragmatic** answer, not a complete one: the precise merge semantics
when many unresolved drvs collapse to one resolved key, and the interaction with
trust (a client authorized for unresolved-drv-A but not unresolved-drv-B that
resolve to the same key) need the human decision flagged in §6. The spike's
prototype (§4) targets the **input-addressed** path first, where the resolved key
is known up front and the `resolving` pre-state is unnecessary — proving the core
machinery before taking on the CA race.

### 3.9 `QueryActiveBuilds` (RFC §4.3.2)

Served entirely by the coordinator (the only component with cross-connection
visibility — a single child cannot see sibling children's builds, §1.1). Returns,
per in-flight `Build` the caller is authorized to see (§3.7): resolved-drv key,
start time, current phase/last activity, subscriber count, bytes of log so far.

---

## 4. Prototype scope and success criteria

> The prototype below is **Workstream A** of the consolidated
> [validation plan](./remote-build-protocol-redesign.validation.md), which folds
> in the O1–O5 acceptance criteria and sequences this against the trust tests
> (Workstream B), the cancel matrix (Workstream C), and serve diagnostic core (Workstream D).

### 4.1 The smallest end-to-end demonstration

The prototype proves **start-or-attach + live log fan-out + replay** on the
**stock fork-per-connection daemon** for **two concurrent clients building the
same input-addressed drv**. Concretely:

1. A **throw-away coordinator binary** (or `nix-daemon --coordinator` sub-role)
   that: listens on the control socket; implements `START_OR_ATTACH` /
   `SUBSCRIBE` / `UNSUBSCRIBE` / `CANCEL_HINT` / `QUERY_ACTIVE` (§3.2);
   keeps the registry, in-memory replay buffer (§3.5), and refcount; runs the
   build by `fork()`ing the builder via the existing `Worker`/
   `DerivationBuildingGoal` path (so output `PathLocks`, §1.3, still apply).
2. A **feature-flagged relay path in the daemon child**: when an experimental
   setting is on and the request negotiated the capability, the child does
   `START_OR_ATTACH` and relays `FRAME`s to its client socket instead of building
   locally; `MonitorFdHup` (§1.5) is rewired to `UNSUBSCRIBE` instead of
   `triggerInterrupt`.
3. A test derivation with a **deliberately slow builder** (e.g. a `sleep`-padded
   script that emits known marker lines and increments a counter file) so the
   second client reliably attaches mid-build.

No public wire changes; the client sees ordinary `STDERR_*` frames (§3.4).

### 4.2 The dedup test (mirrors RFC §9's dedup test)

The pass/fail bar, mirroring RFC §9 "Dedup test":

```
# Pseudocode for tests/functional/build-dedup-coordinator.sh (experimental)
start coordinator
client1: nix build "$slowDrv^*" --store <daemon> -L  &     # miss → starts the build
wait until builder counter file shows the build started
client2: nix build "$slowDrv^*" --store <daemon> -L  &     # hit → attaches

wait both
assert builder_counter == 1                                # EXACTLY ONE build ran
assert client1 saw the live log from the start
assert client2 saw: replayed lines (marked replayed=true) THEN the live tail
assert client2's BuildResult.deduplicated == true
assert client1's BuildResult.deduplicated == false
assert both clients got identical outputs / BuildResult status
```

Additional prototype assertions (proving the hard parts, not just the happy
path):

* **Refcounted cancel (§3.4):** with two clients attached, kill client1; assert
  the build **keeps running** and client2 still completes. Then with only one
  client, disconnect it; assert the build **is cancelled** (counter does not
  advance to completion) — proving the inverted `MonitorFdHup` semantics.
* **Replay correctness (§3.5):** assert client2's replayed prefix + live tail,
  concatenated, equals client1's full stream (no gap/dup at the seam).
* **Backpressure (§3.6):** a client that stops reading mid-build does **not**
  stall the builder (assert the counter file keeps advancing) and gets demoted,
  not the build.
* **Trust (§3.7, mirrors RFC §9 "Trust tests"):** an unauthorized client's
  `START_OR_ATTACH` / `QUERY_ACTIVE` for a build it could not itself request is
  rejected **before** subscription — it cannot read the log or see the drv name.
* **Socket authentication (§3.7.1):** a process connecting to the control socket
  as a *different* uid is refused at the peer-cred check, before any
  `sessionAuth` is read.
* **Coordinator throughput (measurement, not pass/fail; §2.3):** run dozens of
  parallel builds, each with several attached subscribers, and **record the
  coordinator's CPU and per-frame fan-out cost**. This is data to inform the
  open question of whether a single event loop suffices or the coordinator needs
  a sharded/multi-threaded design — not a gate on the spike.

### 4.3 Explicitly out of scope for the prototype

* **No production C++ / no merged code.** Throw-away branch only.
* **CA `resolving` pre-state (§3.8)** beyond a design note — the prototype targets
  the input-addressed path where the resolved key is known up front.
* **Session re-attach after a fully dropped connection (§4.7.4).** The prototype
  proves the coordinator *can* own a build past one subscriber leaving; a *new*
  client process re-attaching by session id is Phase 6, not the spike.
* **Coordinator crash/restart recovery** beyond demonstrating the degrade-to-
  `PathLocks` safety claim (§2.2) by inspection — full supervision/restart is
  productionization.
* **`QueryActiveBuilds` UX / `nix` subcommand** (RFC Phase 5).
* **Any serve-protocol bridge work** — the prototype is worker-protocol/daemon
  only. Serve framing (RFC §4.2 bridge, Phase 2) is independent.
* **Wire freezing.** The prototype informs the Phase 3 wire; it does not freeze
  it (and per RFC §7 / Q4 nothing freezes until the Hydra field set is agreed).

### 4.4 What success means

The spike **succeeds** if the prototype demonstrates all of §4.2 on the stock
daemon, and the team can therefore answer RFC Q0 ("coordinator") and Q1 ("replay
buffer in coordinator memory, head+tail cap") with evidence, and freeze the
Phase 3 *internal* coordinator interface (§3) — leaving only the public-wire
field set (Q4, with Hydra) outstanding. It **fails** (and we revisit M2/M3) if
the prototype shows the coordinator cannot preserve build isolation, cannot
enforce trust before subscribe, or cannot keep the build decoupled from slow
clients.

---

## 5. Confirming the RFC's promised constraints

### 5.1 No Hydra / Nix flag day (RFC §4.8, §7)

The coordinator is **invisible to the wire**. The client-facing worker/serve
protocol frames are byte-identical (§3.4); a child that cannot reach a
coordinator (none running, or capability not negotiated) **builds locally exactly
as today** (§1.1). Therefore:

* Old client ↔ new daemon: client never negotiates the capability → child builds
  locally → today's behaviour. ✅
* New client ↔ old daemon: daemon has no coordinator path → builds locally,
  client gets ordinary frames. ✅
* Hydra (serve `BasicClientConnection`, `serve-protocol.hh:96-101`) is untouched
  by the spike — the prototype is worker-protocol only and additive. ✅

No coordinated upgrade is required; the coordinator is a pure server-side
optimization behind an opt-in capability.

### 5.2 `--keep-going` / root-reason interaction (RFC §10 Q2)

`build.hasRootReasonToContinue()` (§3.4) is where `--keep-going`, an explicit
build root, or a timeout keeps a build alive even at refcount zero. The prototype
stubs this to "false" (cancel at zero) and the §4.2 cancel test exercises the
refcount path; the full Q2 matrix (originating session detaches, late joiners
remain, `--keep-going` on/off, timeouts) is enumerated for Phase 3, not solved
here. **This is left to a human decision (§6).**

### 5.3 Single-process / elastic backends need none of this (RFC §4.3, §4.7)

Single-process multiplexed backends (nixbuild.net-style endpoints, a single
`RemoteStore` connection driving many builds) already have a shared address space,
so they implement the registry/broadcaster/refcount **natively** and proceed in
parallel — they do **not** wait on this spike. The coordinator exists **only** to
give the stock fork-per-connection daemon the same semantics those backends get
for free. Critically, the **Build Session wire surface (RFC §4.1) is identical for
both backend classes**: a client cannot tell whether the `STDERR_*` frames and
`deduplicated` result came from a single-process backend's in-memory broadcaster
or from a stock daemon's coordinator-relayed child. That is the whole point of
confining the mechanism below the wire.

This also suggests a **cheaper de-risking sequence** (sequencing, not extra
work): because both backend classes exercise the *same* Build Session wire,
validating the **wire/Session surface** against a single-process backend's
in-memory broadcaster **first** would prove out the Phase 3 framing before the
coordinator is built — turning the harder coordinator prototype into a pure
mechanism validation rather than also a wire validation.

### 5.4 Items that feed back into the RFC

Surfaced by this spike; **do not** modify the RFC, recorded here per the spike's
charter:

1. **Where resolution happens vs. the build key.** §2.2/§3.8 assume the resolved
   drv (the build key) is available to the coordinator because resolution is
   done client-side before the request, or the build store already holds the
   inputs. The RFC (§4.3 "Build key", §10 Q3) should state **explicitly** whether
   resolution is a client-side precondition of `START_OR_ATTACH` or something the
   coordinator may perform — these have different trust and racing consequences
   (a coordinator that resolves needs eval-store inputs it may not have).
2. **Trust under CA key-merging.** §3.8: when unresolved drvs A and B (one
   authorized for the caller, one not) resolve to the **same** key, the merge
   must not let A's caller read B's content or vice versa. RFC §6 + §10 Q7
   (reuse-key vs. trust) should be specified **together** with the §3.8 merge
   semantics; the spike flags but does not resolve this.
3. **Coordinator as a `nix-daemon` role vs. sidecar.** ✅ **RESOLVED**
   (decisions record,
   [O1](./remote-build-protocol-redesign.decisions.md#operational-decision-o1--coordinator-deployment-model-spike-6-q1)).
   The coordinator is the **daemon binary in a `--coordinator` role** — a
   separate process (not a separate codebase, not in-process with the listener),
   **supervised and reaped by the long-lived `daemonLoop` parent**; one per store
   at `$NIX_STATE_DIR/coordinator.socket` (mode `0660`, peer-cred-verified), run
   as root and **one per machine, not per-user**; lazy spawn (§3.1) is retained
   only as the no-daemon fallback. Whether the coordinator must *survive* a
   daemon restart is deferred to the crash-recovery posture (§6 Q5).

---

## 6. Recommendation and open questions for a human

**Recommendation.** Adopt **Mechanism 1, the coordinator process**, as the
cross-process coordination mechanism for the stock `nix-daemon`, and build the
throw-away prototype of §4 to validate it before freezing any Phase 3 wire
surface. The coordinator is the only candidate that preserves today's
*builder-crash* isolation (builders stay in `fork()`ed subprocesses while a small
supervisory coordinator owns the registry, replay buffer, and refcounts) — with
the honest caveat that a bug in the coordinator's own `Worker`/scheduling code is
a shared crash domain across connections, the same class M3 carries, mitigated
only by keeping the coordinator small (§2.2) — and
enforces the RFC §6 trust check *before* subscription using the **existing**
trust code in **one** place rather than an ad-hoc ACL over a shared artifact,
**behind a peer-credential-verified, permissioned control socket (§3.7.1) so
the per-subscribe check cannot be bypassed by a forged `sessionAuth`**,
keeps the build decoupled from slow clients via per-socket backpressure, and —
decisively — leaves the client-facing wire byte-identical so the Phase 3 framing
can be designed independently of the mechanism and so single-process/elastic
backends and Hydra are entirely unaffected (no flag day). It resolves RFC Q1 by
**placing the bounded replay buffer (full log up to a head+tail cap) in the
coordinator's own memory**, which is the one location that outlives every
connection, and it gives a pragmatic answer to Q3 (key on the resolved drv, with
a short-lived `resolving` pre-state to absorb the CA resolution race). Shared
memory (M2) reintroduces a coordinator under the guise of crash-recovery
bookkeeping with a worse trust story, and single-process (M3) is the cleanest
end-state but the worst on fault isolation and far too large to gate Phase 3 —
it is its own RFC, not a spike.

**Open questions that still need a human decision:**

1. **Coordinator deployment model (RFC §4.3.3.1).** ✅ **RESOLVED** — decisions
   record
   [O1](./remote-build-protocol-redesign.decisions.md#operational-decision-o1--coordinator-deployment-model-spike-6-q1):
   daemon binary in a `--coordinator` role, a separate process supervised/reaped
   by the `daemonLoop` parent, one per store at `$NIX_STATE_DIR/coordinator.socket`
   (`0660`, peer-cred-verified), run as root and one per machine (not per-user),
   lazy spawn as the no-daemon fallback. (Daemon-restart survival is deferred to
   Q5, crash-recovery posture.)
2. **CA key-merge + trust (RFC §6, Q3, Q7) — §3.8/§5.4.2.** Exact semantics when
   distinct unresolved drvs (with *different* per-caller authorization) resolve to
   the same build key, and whether resolution is a client-side precondition or a
   coordinator capability (§5.4.1). This is the thorniest correctness/security
   coupling and must be decided with the trust model, not unilaterally.
3. **Refcounted-cancel matrix with `--keep-going` and timeouts (RFC Q2) — §5.2.**
   The behaviour when the originating session detaches but joiners remain is
   settled (build continues); the interaction with `--keep-going`, explicit build
   roots, and per-build timeouts needs an agreed matrix before Phase 3.
4. **Replay cap default and truncation UX (RFC Q1) — §3.5.** ✅ **RESOLVED** —
   decisions record
   [O5](./remote-build-protocol-redesign.decisions.md#operational-decision-o5--replay-cap-default-and-truncation-ux-rfc-q1-remainder-spike-6-q4):
   byte cap (default 4 MiB), head (~1 MiB) + tail (~3 MiB) with an explicit
   truncation-marker frame, `replayed=true` tagging, post-build handoff to the
   persisted log via `QueryBuildLog`. Cap and split are configurable/tunable.
5. **Coordinator crash-recovery posture.** ✅ **RESOLVED** — decisions record
   [O2](./remote-build-protocol-redesign.decisions.md#operational-decision-o2--coordinator-crash-recovery-posture-spike-6-q5):
   **v1 is safe-degrade, no persistence/re-adoption.** Builders are forked
   `dieWithParent`/in a coordinator-killable cgroup so a crash releases their
   `PathLocks`; relay children fall back to building locally (coalescing via
   `PathLocks`); correctness is guaranteed by the existing lock/validity logic
   (the spike's §2.2 floor). Persistent registry + re-adoption is deferred as a
   separate, evidence-gated hardening item, **not** a Phase 3 prerequisite.
6. **`QueryActiveBuilds` default privacy for untrusted callers (RFC Q5).**
   ✅ **RESOLVED** — decisions record
   [O6](./remote-build-protocol-redesign.decisions.md#operational-decision-o6--queryactivebuilds-privacy-default-rfc-q5-spike-6-q6):
   default is own-authorized-builds-only with no count; an operator may opt in to
   an anonymized aggregate in-flight count. Preserves the no-existence-oracle
   property; enforced at the single `QUERY_ACTIVE` chokepoint (§3.7.3).
7. **Coordinator throughput and concurrency ceiling — §2.3.** ✅ **RESOLVED
   (posture)** — decisions record
   [O4](./remote-build-protocol-redesign.decisions.md#operational-decision-o4--coordinator-throughput-posture-spike-6-q7):
   v1 ships a **single-threaded event loop**; sharding (natural axis: the build
   key) is deferred and **gated on the §4.2 throughput measurement**. Below the
   wire, so a later sharded design is an internal change with no back-compat
   impact.
8. **Lazy-spawn lifecycle posture — §2.4/§3.1.** ✅ **RESOLVED** — decisions
   record
   [O3](./remote-build-protocol-redesign.decisions.md#operational-decision-o3--lazy-spawn-lifecycle-spike-6-q8):
   within one daemon the parent is the sole spawner (lazy, at first
   capability-negotiated request); the fallback uses a `flock`/`O_EXCL`-lockfile
   + socket-`bind` election, stale-socket reclaim, a configurable idle-exit grace
   (default 10 min), and a `GOING_AWAY`/decline-and-respawn handshake.
