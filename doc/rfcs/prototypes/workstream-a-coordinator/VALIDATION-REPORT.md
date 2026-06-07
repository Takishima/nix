# Workstream A — validation report (F-INT freeze-readiness)

> Throw-away prototype review against spike §3/§4, the §8.1 forward-compat
> guardrails, and the validation.md Workstream-A acceptance table. Performed on
> a stock-model `fork()`-per-connection daemon (`reldaemon.cc`) + a real,
> separate coordinator process (`coordinator.cc`), GCC 13, Linux 6.18.

## Verdict

**The prototype exists, builds cleanly (`-Wall -Wextra`, no warnings), and all
three suites pass:** `make check` (A), `make check-b` (B/T1–T3),
`make check-c` (C-a…C-f). The acceptance criteria are demonstrated by *real*
mechanisms, not stubs. **F-INT (the Phase 3 internal coordinator interface,
spike §3) is supported by the evidence and is freezable.**

## What is genuinely proven (not modelled)

| Criterion | Evidence | Real mechanism |
|---|---|---|
| **A-dedup** | `a-dedup.sh`: builder counter == 1 for two concurrent clients | one `Build` per registry key; second `START_OR_ATTACH` is a refcount++ HIT |
| **A-replay** | `a-replay.sh`: client2 `replayed>0` prefix + live tail **byte-identical** (`diff`) to client1's full stream | single-threaded snapshot-then-register seam in `onSubscribe` → no gap/dup (O5 head+tail buffer) |
| **A-backpressure** | `a-backpressure.sh`: builder runs to completion while a stalled client is **demoted** | non-blocking per-socket `outbuf`; `OUT_CAP` overflow → demote, never throttle the build |
| **A-sockauth** | `a-sockauth.sh`: wrong-uid peer **refused before any `sessionAuth` is read** | real `SO_PEERCRED` check in `onAccept`, *before* `dispatch`; authorize re-derived before registry lookup → no existence oracle |
| **A-crash** | `a-crash.sh`: zero orphan builders (ppid==1 scan of `/proc`), exactly one rebuild (counter==2), respawn | real `PR_SET_PDEATHSIG`; relay EOF→`fallbackLocalBuild` coalesced by a real `flock` (PathLock stand-in) |
| **A-spawn** | `a-spawn.sh`: 8 racers → exactly one coordinator + one socket; idle-exit; respawn | real `O_EXCL` lockfile + `bind` election; `unlink`-before-`bind` reclaims a stale socket |
| **A-throughput** | `a-throughput.sh`: 64 builds × 4 subs → 64/64 deduped, ≈0.2 CPU-s single-threaded | measurement only (informs O4); well under any sharding ceiling |

`MonitorFdHup` is correctly **inverted** (spike §1.5 → §3.4): a client HUP makes
the relay child send `UNSUBSCRIBE`/`CANCEL_HINT`, *not* `triggerInterrupt`, so the
coordinator keeps the build alive for other subscribers (guardrail §8.1 #3 —
refcount even at count 1).

## What is modelled / stubbed (honest scoping, per README "Faithful vs. modelled")

- **Log frames** are opaque builder-stdout chunks, not real `STDERR_*`
  worker-protocol frames. Coordination is under test, not framing (§5.1).
- **`authorize()`** is a toy policy (trusted⇒any; untrusted⇒`ca:`/`allow=`),
  sufficient for authorize-before-subscribe and the no-existence-oracle property,
  not the real ACL.
- **A-sockauth** forces the uid mismatch via `WSA_FAKE_DAEMON_UID` (no second real
  user needed) — the *refusal path itself* is real `SO_PEERCRED`.
- **The builder** is `slow-builder.sh` with a counter file; "exactly one build
  ran" is read from that counter, not from store validity.
- **CA resolution** (Workstream B) is a timer (`resolveMs`), not a real resolver;
  what is tested is the `resolving`→promote→re-auth→merge state machine (§3.8).

## §8.1 guardrail compliance (the ones a coordinator can violate)

1. **Resolved-drv key** ✅ registry keys on the resolved drv (provisional
   `unresolved:` entry promoted to the resolved key); no store-path shortcut.
2. **Registry interface vs. transport** ✅ `authorizeFor` is separate from the
   peer-cred transport check (peer-cred on accept; authorize re-derived).
3. **Refcount cancel even at count 1** ✅ `refcount`/`onUnsubscribe`; HUP→UNSUBSCRIBE.
6. **No same-host/store assumption baked into correctness** ✅ dedup correctness
   lives in the registry; `flock` fallback is a degrade path, not the only mechanism.
8. **No un-shardable global state** ✅ one registry keyed solely on the build key;
   no process-wide singletons keyed on anything else.

(#4 build-remote slot locking, #5 activity tagging, #7 serve fields are out of
this prototype's surface — A doesn't touch the hook or the serve wire.)

## Deviations found and corrected in this pass

- **Throughput scale.** The acceptance criterion says "**≥64** parallel builds";
  the test defaulted to 32. Raised `a-throughput.sh` default to 64 (still
  overridable via `WSA_TP_BUILDS`). At 64×4 the coordinator stays ≈0.2 CPU-s.
- **Doc CPU figure.** validation.md claimed "≈0.09 CPU-s across 32 builds"; the
  reproducible figure here is ≈0.2 CPU-s at the corrected 64-build scale (exact
  number is host-dependent). Updated the doc; the conclusion — far under any O4
  sharding ceiling — is unchanged.

No correctness deviations were found; every other claim in validation.md /
README matched observed reality.

## Out of scope (correctly absent, per spike §4.3)

CA `resolving`→promote is **Workstream B** (present here, tested by `check-b`);
session re-attach after a fully dropped connection is **Phase 6**; full
crash/restart re-adoption beyond degrade-to-PathLocks; `QueryActiveBuilds` UX;
any serve-protocol bridge; and **wire freezing** (this informs Phase 3, it does
not freeze the public wire — that is F-WIRE, blocked on the Hydra field set).

## Bottom line

F-INT's engineering precondition is met with running code on a real
fork-per-connection model. Productionizing the interface into `libstore`/`daemon`
is Phase 3 *implementation*, not a freeze gate.
