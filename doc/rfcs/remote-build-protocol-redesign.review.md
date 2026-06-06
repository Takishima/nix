# Review: RFC "A production-ready, introspectable remote build protocol"

This is a review of [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md).
It covers (1) the accuracy of the RFC's findings about the current code and
(2) the soundness of the architecture it proposes. All code references were
checked against the tree at the time of writing.

## Verdict

A strong, well-researched RFC. The **diagnosis is accurate** — the code
references hold up to within a line or two — and the **phasing front-loads
real value** (Gap A first). However, the **central mechanism of the headline
feature (§4.3 dedup/attach) rests on an assumption about the daemon's process
model that is false**, and the RFC does not acknowledge the gap. The
operational fixes (Gaps A/B/C, Phases 0–2/4) are ready to proceed; the
dedup/attach/reattach story (§2.3, §4.3, §4.7.4) needs revision.

## 1. Findings accuracy

Nearly every citation was verified and is correct, including:

| RFC claim | Location | Status |
|---|---|---|
| Serve version 2.8, `SERVE_PROTOCOL_VERSION (2 << 8 \| 8)` | `serve-protocol.hh:11` | ✅ |
| Command list `QueryValidPaths`…`AddToStoreNar` | `serve-protocol.hh` | ✅ |
| `BasicClientConnection`/`BasicServerConnection` "for sharing with Hydra" | `serve-protocol.hh` | ✅ |
| `getBuildLogExact` → `unsupported`, `FIXME extend daemon protocol` | `ssh-store.cc:64-67` | ✅ |
| `mounted-ssh-ng` delegates `getBuildLogExact` to `LocalFSStore` | `ssh-store.cc:181` | ✅ |
| Worker protocol `STDERR_*` framing | `worker-protocol.hh:22-31` | ✅ |
| Worker `PROTOCOL_VERSION 1.39` | `worker-protocol.hh:18` | ✅ |
| `opServe`/`getBuildSettings` sets `verbosity = lvlError` + `keepLog = false` | `nix-store.cc:908-909` | ✅ |
| `BuildDerivation` "Used by hydra-queue-runner" | `nix-store.cc:1020` | ✅ |
| `MonitorFdHup` on the serve build path | `nix-store.cc:1009,1032` | ✅ |
| Hook slot/file locking + `maxJobs` | `build-remote.cc:40-42,153,168` | ✅ |
| Failure thrown from `BuildResult::Failure::message()` | `build-remote.cc:351` | ✅ |
| Trust comment cross-refs `daemon.cc` `BuildDerivation` | `build-remote.cc:324-329` | ✅ |
| `copyDrvsFromEvalStore` | `remote-store.cc:548,571,584` | ✅ |
| `fixupBuilderFailureErrorMessage` carries the tail | `derivation-building-goal.cc:1157` | ✅ |
| ssh-ng hook forwards `resBuildLogLine`/`resSetPhase` | `derivation-building-goal.cc:744-746` | ✅ |
| `printBuildLogs` gates `resBuildLogLine` output | `progress-bar.cc:345-362`; `main.cc:117` | ✅ |
| `addBuildLog`, `TunnelLogger`, `min()` handshake | `local-store.cc:1629`, `daemon.cc:50,1054`, `serve-protocol-connection.cc:20,32` | ✅ |
| In-process dedup via `initGoalIfNeeded` | `worker.cc:59` | ✅ |

The validated log-flow matrix (§2.6) and Gaps A/B/C are real and correctly
characterized. This is the strongest part of the document.

**Minor corrections:**

- `ssh://`+`--eval-store` rejection is at **`legacy-ssh-store.cc:222`**, not
  `:221`.
- The distributed-build hook lives at **`src/nix/build-remote/build-remote.cc`**,
  not in `src/libstore/`. The RFC cites it as bare `build-remote.cc`, which is
  mildly misleading since most neighbouring refs are in `libstore`.
- `get-build-log.cc` is in **`src/libcmd/`**, not `src/nix/`.

## 2. Major architectural gap: §4.3 assumes a shared `Worker`; the daemon forks per connection

§4.3 places the Build Registry, log broadcaster, and replay buffer in "the
builder's **shared `Worker`/daemon, not per-connection**," and §2.3 asserts
that two connections to a shared `nix-daemon` make "the daemon's `Worker`
**share one goal**."

This is not how the daemon works. `daemonLoop`
(`src/nix/unix/daemon.cc:352-382`) **forks a separate child process per
connection** (`// Fork a child to handle the connection.`); each child calls
`storeConfig->openStore()` and runs its own `processConnection` with its own
`Worker` in its own address space. There is no `Worker`, registry, or buffer
shared across connections.

What coalesces two concurrent builds of the same derivation today is
**filesystem `PathLocks` on the output paths**, not goal-sharing. The second
process blocks on the lock and finds the outputs valid when it wakes. That
serializes redundant *work* but provides **no log fan-out**, is not the
in-process `initGoalIfNeeded` dedup the RFC says it "generalises," and makes
§2.3's "shares one goal" claim incorrect for two separate connections.

Consequences:

1. **Phase 3 is badly understated.** Cross-connection dedup/attach/fan-out
   requires one of: (a) a coordinator process the forked children talk to over
   IPC/a socket; (b) shared memory plus a published log ring buffer keyed on
   the resolved drv; or (c) re-architecting the daemon away from
   fork-per-connection to a single-process multiplexed model. Each is a major
   design effort and deserves its own section. The "Touches" list
   (`build/worker.{cc,hh}`, new `build-registry.{cc,hh}`) implies an in-process
   data structure that cannot exist here.
2. **§4.7.4 "Reconnection for long builds" inherits the problem.** A build
   outliving its connection is impossible in the fork model: client disconnect
   triggers `MonitorFdHup` (`nix-store.cc:1009`) and the child exits. Re-attach
   presupposes the same daemon redesign.
3. **Reference-counted cancellation (§4.3)** is an in-process concept that does
   not survive process boundaries without the coordinator.

The *goal* — one build, many followers, with replay — is sound and valuable.
The RFC should either (a) scope dedup/attach explicitly to single-process
multiplexed backends (nixbuild.net-style endpoints, or a single `RemoteStore`
connection driving many builds) and state plainly that the stock
fork-per-connection daemon won't get cross-client dedup without further work;
or (b) add a section proposing the cross-process coordination mechanism.

## 3. Strengths

- **Option B via Option A is the right destination/bridge.** Reusing the
  proven worker-protocol streaming framing instead of inventing a second one is
  correct, and keeping serve additive for Hydra (G7) via the existing `min()`
  handshake matches how the code already gates 2.3/2.6/2.7/2.8 fields.
- **Phase 0 is excellently chosen.** Closing Gap A (`getBuildLogExact` over the
  worker protocol) is small, self-contained, high-leverage, and implements a
  standing `FIXME`. Shipping it first earns trust for the rest.
- **Phase 1 is decoupled from streaming and independently valuable.** Carrying
  failure tail/phase/exit as *fields* rather than a baked-in string is the
  right modeling; `print-build-logs = on-failure` is the correct CI ergonomic.
- **Resolved-derivation dedup key** is correct for CA and matches
  nixbuild.net's reuse semantics; Q3 rightly flags the resolution race.
- **Trust (§6)** correctly identifies that attach must check per-session
  authorization *before* subscribing, or dedup becomes a cross-tenant
  log-exfiltration oracle.

## 4. Other risks

- **Activity tagging (§4.2)** needs more than a sentence: when one session
  drives N drvs whose activity trees interleave, reliable attribution of every
  sub-activity to a resolved drv is the difference between a legible stream and
  log soup.
- **Replay buffer (Q1)** location is undecided precisely because there is no
  shared address space (see §2 above).
- **Gap B fix (Phase 2)** — feeding parsed log lines into `worker.act`/`logger`
  rather than only `logFile->sink` is right; watch for double-logging when the
  same daemon both drives the hook and is the ambient logger.
- **`maxJobs` as a hint (§4.7.1)** changes scheduling behaviour for existing
  `/etc/nix/machines` files; operators rely on it as a hard cap. This must be
  opt-in, not a default.
- **Hydra field freeze (Q4)** is a hard external dependency: Phases 1/2 must
  not freeze the serve 3.0 wire until that coordination lands, or back-compat
  promises could be broken retroactively.

## 5. Phasing

Ordering is good and each phase is shippable **except Phase 3**, which is a
sub-project gated on a daemon process-model decision. Recommended:

- Insert a **design spike** (cross-connection build coordination) that picks
  the mechanism *before* freezing Phase 3's wire surface, since the choice
  bounds what `QueryActiveBuilds` and session re-attach can promise.
- State explicitly that Phases 0, 1, 2, 4 deliver Gaps A/B/C — the bulk of the
  operator pain — independently of the shared registry, as the "even if dedup
  never lands" fallback.

## Bottom line

Approve the diagnosis and Phases 0–2/4 enthusiastically. Send §2.3, §4.3, and
§4.7.4 back for revision: the shared-`Worker` premise is false under the
fork-per-connection daemon (`daemon.cc:352-382`), so dedup/attach/replay/
reattach needs either explicit scoping to single-process backends or a new
section proposing cross-process coordination. Fix the three line/path nits
while in there.

---

## Response (RFC author)

Thank you — the fork-per-connection finding is correct and was the right
thing to catch. I verified it independently before revising:
`startProcess(...)` in `daemonLoop` forks a child per connection, each doing
its own `openStore()` (`src/nix/unix/daemon.cc`), and cross-process
coalescing is via output `PathLocks`
(`derivation-building-goal.cc:437-497`, `outputLocks.lockPaths(...)`), with
no log fan-out. The RFC has been revised accordingly; I took option (a) **and**
(b) — explicit scoping *and* a proposed mechanism.

**Major gap (§2 of review) — addressed:**

- **§2.3 rewritten** to state the daemon is fork-per-connection with no
  shared `Worker`, that coalescing is by output `PathLocks` (not
  goal-sharing), and that this provides no log fan-out. The false "shares
  one goal" claim is removed.
- **§4.3 rewritten** with a scope note up front: it is the one feature that
  is *not* a thin exposure of existing capability, is conditional on
  §4.3.3, and splits into single-process backends (native, first) vs. the
  stock daemon (needs the coordinator). The "generalises `initGoalIfNeeded`"
  claim is corrected.
- **New §4.3.3 "Cross-process build coordination"** proposes the three
  candidate mechanisms (coordinator process / shared memory / single-process
  daemon), recommends the coordinator as the starting point, and marks the
  choice as a gated design spike.
- **§4.7.4 re-attach** now states plainly it is impossible in the stock
  daemon (`MonitorFdHup` → child exits) and depends on §4.3.3.
- **Reference-counted cancellation** and the **replay buffer location** are
  re-scoped to depend on §4.3.3.

**Phasing (§5 of review) — addressed:** a **design spike** is inserted
before Phase 3; the plan now states explicitly that Phases 0–2/4 deliver
Gaps A/B/C with no shared state, as the "even if dedup never lands" core;
Phase 3 is split into single-process vs. coordinator tracks and its
"Touches" no longer implies a simple in-process structure.

**Other risks (§4 of review) — addressed:**

- **Activity tagging (§4.2)** expanded from a sentence into the load-bearing
  detail: a per-build root activity, `parent`-based attribution, and a
  client-side `ActivityId → build id` index (also used by the on-failure
  dump).
- **`maxJobs` as a hint (§4.7.1)** is now explicitly **opt-in**; default
  hard-cap semantics are unchanged.
- **Gap B double-logging (Phase 2)** caveat added.
- **Hydra field freeze (Q4)** added to §7 as an explicit rule: do not freeze
  the serve 3.0 wire until the Hydra field set is agreed; prototype behind an
  unstable version meanwhile.
- **Replay buffer (Q1)** now cross-references §4.3.3 for its location.
- A new **Q0** names the cross-process coordination mechanism as the
  blocking open question.

**Nits — fixed:** `legacy-ssh-store.cc:221`→`:222` (all occurrences);
`get-build-log.cc`→`libcmd/get-build-log.cc`; the distributed-build hook's
full path `src/nix/build-remote/build-remote.cc` is now noted (see §2.1).
