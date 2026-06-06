# Review: Spike "Cross-process build coordination for dedup/attach/replay"

This is a review of
[`remote-build-protocol-redesign.spike.md`](./remote-build-protocol-redesign.spike.md),
which itself responds to the
[RFC review](./remote-build-protocol-redesign.review.md)'s §2 finding that
the daemon is fork-per-connection. All code references in the spike were
re-verified against the working tree as part of this review.

## Verdict

Strong, honest, and correctly reasoned. The recommendation (Mechanism 1,
the coordinator process) is right; the §1 facts are accurate; the prototype
is genuinely throw-away with pass/fail criteria that mirror RFC §9; and the
headline invariant — **the coordinator sits below the public wire, so no
flag day and the single-process/Hydra paths are untouched** — is the
strongest property in the document.

Before the mechanism is considered *settled*, two things must be addressed:
(1) the fault-isolation claim is overstated and overlaps with M3 for the
`Worker`-logic layer, and — more importantly — (2) **the coordinator
socket's own authentication is unspecified**, which is a real security gap,
not a nuance. Two smaller items (coordinator throughput, lazy-spawn races)
should be named as open questions.

## 1. Findings accuracy

The spike's §1 "load-bearing facts" were independently re-verified; all hold:

| Spike claim | Citation | Status |
|---|---|---|
| `daemonLoop` accepts then forks | `src/nix/unix/daemon.cc:247`, `:352` | ✅ |
| Each child opens its own store | `src/nix/unix/daemon.cc:376` | ✅ |
| Child runs `processConnection` then `exit(0)` | `daemon.cc:378,380`; `libstore/daemon.cc:1022` | ✅ |
| `authPeer` is how the child authenticates | `daemon.cc:342` (call); def at `:212` | ✅ (see nit) |
| In-process dedup = `initGoalIfNeeded` | `worker.cc:59` | ✅ |
| Goal map is per-`Worker` | `worker.hh:123` | ✅ |
| `Worker` holds build + eval store | `worker.hh:200-201`, ctor `:236` | ✅ (see nit) |
| Cross-conn coalescing = output `PathLocks` | `derivation-building-goal.cc:437,470,482,496-497` | ✅ |
| Log → client via `TunnelLogger`/`STDERR_*` | `daemon.cc:50,70,95-96,114,129,136` | ✅ |
| Client consumes via `processStderr`; `parent` on activity | `remote-store.cc:166`; `worker-protocol-connection.cc:78,84-85` | ✅ |
| Disconnect → `triggerInterrupt` → child exits | `monitor-fd.hh:20,120` (Linux), `:93` (Apple) | ✅ |
| `getBuildLogExact` unsupported over ssh-ng | `ssh-store.cc:64-67` | ✅ |

**Trivial citation nits:** `authPeer` is *defined* at `daemon.cc:212`; the
spike cites its `:342` call site (fine, but worth noting). `Store & store`
is `worker.hh:200` and `Store & evalStore` is `:201`; the spike groups both
at `:201`. Neither affects the argument.

## 2. Substantive gaps (address before declaring the mechanism settled)

### 2.1 The fault-isolation claim is overstated; M1 overlaps M3 for the `Worker` layer

The coordinator "runs the real `Worker`" (§2.2, §3.1) for **all** clients in
one process. Builder *subprocess* crashes stay isolated — the fork-per-build
mitigation is sound and preserves today's containment for sandboxed builder
code. **But** a bug in goal / `Worker` / resolution / scheduling code — the
exact non-sandboxed code path M3 is scored `●` "strictly worse" for
(§2.4) — now crashes the coordinator and takes down **every** client's
build at once. On the "Worker-logic crash" axis, **M1 ≈ M3**, not M1 ≫ M3.

M1 is therefore a *hybrid*: single-process-multiplexed for the
scheduling/`Worker` layer, fork-per-build for builders. The recommendation
still holds (M1 wins decisively on implementation blast radius and
throw-away-ability), but:

- The §2.5 scoring of M1 `◑` vs M3 `●` on fault isolation is generous.
- The claim "preserves today's fault isolation" (§2.2, §6) is true only for
  *builder* crashes and should be qualified as such.

**Recommended fix:** restate the isolation property precisely ("builder
crashes stay contained; a coordinator-`Worker` bug is a shared crash domain
across connections, the same class M3 carries") and adjust the scoring
narrative accordingly. This does not change the recommendation; it makes the
honest trade-off legible.

### 2.2 The coordinator socket's own authentication is unspecified — security linchpin

§3.7 says the coordinator "re-checks before subscribing, never trusting that
the child checked." Two problems:

1. **Authentication vs. authorization are conflated.** The coordinator
   *must* trust the connection child for **authentication** — only the child
   holds the peer's `SO_PEERCRED` / `authPeer` result. It can independently
   re-derive **authorization** (may this identity build this drv). The
   wording "never trusting the child" is too strong and should be split into
   "trust the child's reported authenticated identity; re-derive the
   authorization decision."

2. **Who may talk to the coordinator socket?** This is the real gap. If any
   local process can open `$NIX_STATE_DIR/coordinator.socket` and assert a
   `sessionAuth` ("I am uid X, authorized for drv Y"), the entire RFC §6
   trust model collapses into the cross-tenant exfiltration oracle the spike
   credits M1 for *avoiding* over M2 (§2.3). The spike must specify:
   - the socket's access control (e.g. root- / `nix-daemon`-group-only,
     mode `0660`), and
   - how the coordinator verifies a connecting peer is a legitimate trusted
     daemon child (peer-cred check on the control socket) rather than an
     attacker forging `sessionAuth`.

   Until this is written down, the security story is incomplete. This is the
   single most important thing to fix in the spike.

### 2.3 The coordinator is a new central throughput bottleneck — tension with G6 (scaling)

Every build for every connection funnels through one coordinator event loop,
which also multicasts every log frame to every subscriber
(O(frames × subscribers) in one process). §3.6 handles slow *clients* but not
coordinator *throughput* under high build concurrency — which is exactly the
busy-builder scenario G6 (elastic backends) exists to serve. Ironically, the
mechanism added partly in service of scale introduces a central serialization
point.

Mitigating factor: the per-client relay lives in the children, so the
coordinator's per-frame cost is "append to replay buffer + persisted log +
write to N subscriber fds." Still centralized. **Recommended:** name this as
an open question — whether the coordinator needs a multi-threaded / sharded
event loop, and what its concurrency ceiling is — and have the prototype
record coordinator CPU under, say, dozens of parallel builds.

### 2.4 Lazy-spawn lifecycle races

"Started lazily by the first child that wants it … idle-exits after a grace
period" (§3.1) invites two classic races: (a) two children racing to spawn
the coordinator, and (b) the coordinator idle-exiting between a child's
connect and its first use. Both are solvable (a lock-file or socket-`bind`
election; a "decline-and-respawn" handshake on connect-after-exit), but the
prototype should call them out so a flaky spawn is not misread as a
coordination bug.

## 3. Minor / precision

- "**never trusting that the child checked**" (§3.7) — tighten per §2.2.1.
- §2.2 "the daemon already `fork()`s per build internally" is accurate, but
  note the builder's parent changes from connection-child to coordinator;
  confirm `SIGCHLD`/reaping ownership moves with it (the spike does cover
  reaping — keep it explicit).
- **Cheaper de-risking path (worth one sentence):** since single-process /
  elastic backends need none of this and exercise the *same* Build Session
  wire (§5.3), validating the **wire/Session surface** via a single-process
  backend *first* would de-risk the Phase 3 framing before the coordinator
  is built — sequencing, not extra work.

## 4. Strengths (keep as-is)

- **Mechanism choice is correct**, and the dismissals are fair: M2
  "reintroduces a coordinator under crash-recovery bookkeeping with a worse
  trust story," M3 "cleanest end-state but its own RFC, not a spike."
- **Q1 resolved soundly** — replay buffer in coordinator memory, full log up
  to a head+tail cap — and correctly tied to Q0 (the location *is* the
  mechanism decision).
- **Below-the-wire invariant (§5.1)** and the **identical Session surface
  for both backend classes (§5.3)** are the right architectural commitments
  and are what make the no-flag-day guarantee real.
- **Trust check before subscribe, in one place, using the existing trust
  code** (§3.7) is the right shape — modulo the socket-authentication gap of
  §2.2.
- **Scope discipline:** input-addressed first; CA `resolving` pre-state,
  session re-attach, and coordinator crash-recovery deferred with the right
  items (CA key-merge + trust, deployment model) escalated to humans (§6).
- **Refcounted-cancel design (§3.4)** is the precise inversion of today's
  1:1 `MonitorFdHup` semantics and is exactly what RFC §4.3 needs.

## Bottom line

Approve the direction and the prototype plan. Two required revisions before
the mechanism is "settled": (1) reframe the isolation claim as
builder-crash-only and acknowledge the M1/M3 overlap for `Worker`-logic
crashes (adjust the §2.5 scoring narrative); and — the important one — (2)
**specify the coordinator socket's own access control and peer
verification**, since that, not the per-subscribe check, is where the trust
model actually stands or falls. Add coordinator throughput (§2.3) and
lazy-spawn races (§2.4) to §6's open-questions list. Fix the two trivial
citation nits while in there.
