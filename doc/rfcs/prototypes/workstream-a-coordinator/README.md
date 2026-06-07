# Workstream A + B + C prototype — coordination, CA-merge trust, refcount-cancel

> **Status:** throw-away validation prototype.
> **Gates:** freeze **F-INT** (the Phase 3 *internal* coordinator interface) via
> Workstream **A**; the **F-WIRE precondition** (trust tests T1–T3) via Workstream
> **B**; the **refcounted-cancel matrix** (Blocker 2, coordinator-internal, no
> wire) via Workstream **C**. B and C both extend the A1 prototype.
> **Parent:** [`../../remote-build-protocol-redesign.validation.md`](../../remote-build-protocol-redesign.validation.md)
> (Workstreams A, B & C) · [`../../remote-build-protocol-redesign.spike.md`](../../remote-build-protocol-redesign.spike.md)
> (§3 interface, §3.8 CA resolve/promote, §4 prototype plan) ·
> [`../../remote-build-protocol-redesign.decisions.md`](../../remote-build-protocol-redesign.decisions.md)
> (Blocker 2 cancel table)

This is the experimental branch the validation plan and the spike call for. Its
single job is to **validate**, with running code, the cross-process coordination
mechanism the spike chose (Mechanism 1, the coordinator process) **before any
Phase 3 wire surface is frozen** — and to produce the evidence behind RFC Q0
("coordinator") and Q1 ("replay buffer in coordinator memory, head+tail cap").

It is **deliberately not production code** (spike §4.3): it depends on nothing in
the Nix tree, models the daemon/relay/coordinator topology in ~900 lines of
standalone C++, and is expected to be **deleted wholesale** once the design is
validated. None of it is meant to be merged into `libstore`/`daemon` as-is. What
*does* graduate is the *interface* (§3.2) and the *evidence* that it holds.

## Why this exists (the load-bearing fact)

The RFC's headline feature — one build on a builder, many clients attached to its
live log, with replay for late joiners and reference-counted cancellation — is
blocked on one process-model fact: **the stock `nix-daemon` is
fork-per-connection** (spike §1.1). Two connections share no `Worker`, no goal
map, no log buffer, so "share one build, fan out its log" does not exist even
latently. The coordinator is the component that outlives individual connections
and owns that shared state. This prototype proves it can.

## Topology (spike §3.1)

```
 client A ──unix sock──▶ reldaemon child A ─┐
                          (fork-per-conn)    ├──control sock──▶  coordinator
 client B ──unix sock──▶ reldaemon child B ─┘   (peer-cred       (one per store,
                                                 verified)        long-lived)
                                                                    │
                                              Registry: buildKey → Build
                                              Build = { forked builder subproc,
                                                        replay buffer (head+tail),
                                                        subscriber set, refcount,
                                                        persisted-log writer }
```

The relay child speaks the **public-wire stand-in** to its client (ordinary log
records + a result); it cannot tell — and neither can the client — whether the
bytes came from a coordinator or a local build. That is the spike's no-flag-day
property (§5.1): the mechanism lives entirely *below* the wire.

## Components

| File | Role | Deliverable |
|---|---|---|
| `proto.hh` | control protocol (§3.2), framing, socket + peer-cred helpers | — |
| `coordinator.cc` | the coordinator: registry, replay buffer, refcount, fan-out, trust, single-threaded event loop | **A1** |
| `reldaemon.cc` | fork-per-connection daemon + relay child; lazy-spawn election; crash fallback | **A2** |
| `slow-builder.sh` | deliberately-slow test builder; counter + progress side-effects | **A3** |
| `client.cc` | `nix build` stand-in (IA / CA, normal / slow-reader / disconnect) | test driver |
| `ctl.cc` | direct control-socket probe (`QUERY_ACTIVE` + `--start` for T1/T3) | test driver |
| `tests/` | criteria harness (A) + trust tests (B) + cancel matrix (C) | — |

Workstreams B and C live in the **same** coordinator, because the validation plan
specifies them as **extensions of the A1 prototype**: the CA resolve→promote
machinery is `promote()` / `checkPromotions()` (B, spike §3.8); the per-subscriber
deadlines, keep-failed OR, and scoped cancel are `checkDeadlines()` /
`maybeFinalize()` / `onUnsubscribe()` (C, Blocker 2).

## Build & run

```sh
make            # builds coordinator, reldaemon, client, ctl  (needs g++/clang++, C++20, Linux)
make check      # Workstream A acceptance-criteria harness
make check-b    # Workstream B trust tests (T1-T3), extends A1
make check-c    # Workstream C refcounted-cancel matrix (C-a..C-f)
make check-fwd  # forward-compat inventory tests (R-class, M-arch) — gate no freeze
make check-all  # all four
WSA_DEBUG=1 ./reldaemon /tmp/wsa &                 # manual: start a daemon
./client --state /tmp/wsa -k demo -t 1             # manual: input-addressed build
./client --state /tmp/wsa --ca -U ca:u -V ca:r -M 500 -t 1   # manual: CA build
```

Linux-only: it uses `SO_PEERCRED` (§3.7.1) and `PR_SET_PDEATHSIG` (O2).

## Acceptance criteria → tests

Every criterion from the validation plan's Workstream A table is exercised by a
script under `tests/`. `make check` runs them all; each prints `PASS`/`FAIL`.

| ID | Asserts | Test | Spike |
|---|---|---|---|
| **A-dedup** | two concurrent clients on one key → `counter == 1` | `a-dedup.sh` | §3.3 |
| **A-replay** | late joiner's `replayed=true` prefix + live tail == originator's full stream, no gap/dup at the seam | `a-replay.sh` | §3.5 |
| **A-backpressure** | a stalled client does not stall the builder (progress completes); the slow session is **demoted**, not the build | `a-backpressure.sh` | §3.6 |
| **A-sockauth** | a peer whose uid ≠ daemon uid is refused at the `SO_PEERCRED` check before any `sessionAuth` is read; untrusted start/attach denied; `QUERY_ACTIVE` filtered | `a-sockauth.sh` | §3.7 |
| **A-crash** | kill the coordinator mid-build → no orphan builder (`PR_SET_PDEATHSIG`); relays fall back to local builds coalesced to **exactly one rebuild** (PathLock floor); next build respawns a coordinator | `a-crash.sh` | §5.1 / O2 |
| **A-spawn** | N racing first-requests elect **exactly one** coordinator + one socket; idle-exit; respawn | `a-spawn.sh` | §3.1 / O3 |
| **A-throughput** | measure single-threaded coordinator CPU + fan-out under many parallel builds × subscribers (data, not pass/fail) | `a-throughput.sh` | §2.3 / O4 |

`a-cancel.sh` demonstrates the refcount basics (C-a/C-b/C-c subset); the full
matrix is Workstream C below.

## Workstream B — CA key-merge trust validation (gates F-WIRE)

The **one remaining unproven mechanism** (validation plan, Workstream B): the CA
`resolving`→promote path (spike §3.8) and **re-authorization on promotion**. A CA
build registers a short-lived provisional entry keyed on the *unresolved* drv;
when resolution completes the coordinator promotes/merges it onto the *resolved*
key and **re-authorizes every subscriber against that resolved key**, detaching
failures with an error. `make check-b` runs the three trust tests that are the
**literal F-WIRE precondition**:

| ID | Scenario | Asserts | Test |
|---|---|---|---|
| **T1** | unauthorized `START_OR_ATTACH` for a resolved key that *is* in flight vs one that is *not* | byte-identical denial, no measurable timing difference (authorize runs **before** the registry lookup) → no existence oracle | `b-existence-oracle.sh` |
| **T2** | two distinct unresolved drvs (A authorized, B not for the resolved key) resolving to the **same** key | B detached with an error at promotion; B observes **none** of A's log; one build runs | `b-merge.sh` |
| **T3** | a child asserts a `buildKey` not matching the drv it sent | rejected; the coordinator recomputes the key from the drv material it received | `b-spoof.sh` |

`b-resolve.sh` first proves the happy path: a second client racing the **same
unresolved drv** attaches to the provisional `resolving` entry and both promote
onto one shared resolved build (dedup through the resolve race).

The decisions this validates: `resolving` pre-state keyed on the unresolved drv;
promote/merge onto the resolved key; **re-authorize-on-promotion** (decisions B1
§5); the coordinator-recomputes-the-key rule (never trust the asserted key);
and the no-existence-oracle property (authorize-before-registry).

## Workstream C — refcounted-cancel matrix (Blocker 2, no wire)

Implements `C1` — `hasRootReasonToContinue()` as **explicit-root-only** plus the
**per-subscriber deadline + max-envelope** timer that spike §5.2 stubs to
`false` — and encodes the full `build-dedup-cancel` matrix (decisions B2 §4).
This is **coordinator-internal** (no wire change), so it gates no freeze, but the
table must hold before Phase 3 implementation is trusted. `make check-c`:

| ID | Scenario | Asserts |
|---|---|---|
| **C-a** | two attached, kill #1 (originator) | build continues; #2 completes; one build (no originator privilege) |
| **C-b** | one attached, disconnect, no root | build cancels (progress < total) |
| **C-c** | one attached **with explicit root**, disconnect | build completes |
| **C-d** | two deadlines (short / long) | at the short deadline #1 gets `TimedOut` and detaches; build continues for #2 under the **max envelope** |
| **C-e** | build fails, `--keep-failed` set by exactly one of two | failed dir **preserved** (logical OR); a no-keep-failed control cleans it up |
| **C-f** | active-cancel (`CANCEL_HINT`) by one of two | cancel scoped to the canceller; the other still completes |

Frozen rules exercised (Blocker 2 §3): lifetime = **refcount + explicit-root
only**, no originator privilege; timeout = **per-subscriber detach under a max
envelope** (strictest-wins rejected as a cross-tenant DoS); `--keep-failed` =
**logical OR**; cancel = **scoped unsubscribe-with-error**, no new wire status
(`TimedOut` is modelled as a per-subscriber result, the build is unaffected).

## Forward-compat inventory tests — R-class, M-arch (gate no freeze)

`make check-fwd` runs two of the RFC's elastic-backend inventory tests
(validation.md "Forward-compatibility & elastic-backend additions"). They are
here, in the throw-away prototype, **on purpose**: both exercise Phase-3
*coordinator* semantics (failure classification + retry sizing; the build-key
definition) that have no production code yet, and the §4.4 classification fields
ride the **deferred** serve set — so a `tests/functional` / `src/libstore-tests`
test would have nothing real to drive. They gate **no** freeze.

| ID | Scenario | Asserts |
|---|---|---|
| **R-class** (`tests/r-class.sh`) | builder-internal failure (OOM, exit 137) vs. build-intrinsic failure (exit 1) | OOM → `class=transient` + `hint=memory`; exit 1 → `class=build-error` (distinct, no hint); a re-run of the transiently-failed key starts a **fresh** build (`DEDUP=0`, counter advances) — the failure is **not cached / no key poisoning** (RFC §4.4 / §4.7.5) |
| **M-arch** (`tests/m-arch.sh`) | same package for `x86_64-linux` vs `aarch64-linux`, concurrent on one endpoint | **two distinct keys → two builds, never coalesced**; each client sees only its own system's log; an identical-key **positive control** *does* coalesce to one build (proving distinctness is about the key, not timing) (RFC §4.7; guardrail §8.1 #1) |

To model the deferred failure-classification fields, the client-facing result
record (`CRec::Result`, the public-wire stand-in) carries a `FailClass` +
`resourceHint` — kept **off** the frozen serve 2.9 diagnostic core, next to
`deduplicated`, exactly as decisions Blocker 3 specifies for the deferred set.
The builder (`slow-builder.sh`) gained an optional exit-code argument so a test
can request exit 137 (OOM) vs the default exit 1.

## Which decisions this bakes in (and validates)

- **Mechanism 1, coordinator process** (spike §2.5 recommendation, RFC Q0).
- **Replay buffer in coordinator memory, head+tail byte cap** (RFC Q1, O5):
  `coordinator.cc` `Build::appendReplay` (`WSA_HEAD_CAP` / `WSA_TAIL_CAP`).
- **Single-threaded event loop** (O4): one `poll()` loop; throughput measured so
  a later sharded design (keyed on the build key) stays an internal change.
- **Authenticate the socket, then authorize before subscribe** (§3.7.1/§3.7.2):
  peer-cred uid check on accept; `authorize()` re-derived by the coordinator,
  before touching the registry; denials are existence-oracle-free.
- **Safe-degrade crash posture, no persistence** (O2): builders `PR_SET_PDEATHSIG`
  die with the coordinator; relays fall back to PathLock-coalesced local builds.
- **Lazy-spawn election** (O3): `O_EXCL` lockfile + socket `bind`, idle-exit,
  stale-lock reclaim, decline-and-respawn.

## Faithful vs. modelled (honest scoping)

This is a model, not the daemon. What is **faithful**: the process topology
(real `fork()`-per-connection, a real separate coordinator process), real Unix
sockets, **real `SO_PEERCRED`** auth, real non-blocking single-threaded fan-out
with per-socket backpressure, real `PR_SET_PDEATHSIG` orphan-prevention, real
`flock`-coalesced fallback (the stand-in for output `PathLocks`, §1.3).

What is **modelled / simplified**, on purpose:

- The "log frames" are opaque builder stdout chunks, not real `STDERR_*`
  worker-protocol frames; the relay→client records (`CRec`) stand in for the
  public wire (§5.1). The point under test is the *coordination*, not the framing.
- `authorize()` is a toy policy (trusted ⇒ anything; untrusted ⇒ CA-prefixed
  keys) standing in for the daemon's real trust check. It is enough to prove
  *authorize-before-subscribe* and the no-existence-oracle property; it is not
  the real ACL.
- A-sockauth forces a uid mismatch via `WSA_FAKE_DAEMON_UID` (rather than a
  second real user) to exercise the peer-cred *refusal path* without privileges.
- The builder is a shell script with a counter file; "exactly one build ran" is
  read from that counter rather than from store validity.

- **CA resolution itself is modelled as a timer** (`resolveMs`), not a real
  derivation resolution: the test supplies the unresolved drv, the resolved drv
  material, and how long resolution "takes". What is under test is the
  coordinator's *resolving→promote→re-auth→merge* state machine (spike §3.8), not
  the resolver. The resolved-key allowlist (`allow=<uid,…>`) stands in for "who
  may build the resolved output".

**Out of scope** (spike §4.3), intentionally absent: session re-attach after a
fully dropped connection (Phase 6), full crash/restart re-adoption (only the
degrade-to-PathLocks claim is demonstrated), `QueryActiveBuilds` UX, any
serve-protocol bridge, and **wire freezing** (this *informs* Phase 3, it does not
freeze it). CA `resolving`/promotion, previously out of scope for the A-only
prototype, is now implemented and tested under **Workstream B**.

## Tunables (environment)

`WSA_STATE_DIR`, `WSA_DEBUG`, `WSA_HEAD_CAP`, `WSA_TAIL_CAP`, `WSA_OUT_CAP`
(per-subscriber backpressure cap), `WSA_IDLE_MS` (idle-exit grace),
`WSA_SOCKBUF` (shrink kernel socket buffers for the backpressure test),
`WSA_FAKE_DAEMON_UID` (force the A-sockauth uid mismatch),
`WSA_CLIENT_SLEEP_MS`, `WSA_TP_{BUILDS,SUBS,LINES}` (throughput knobs).

## What success means (validation plan §4.4)

The prototype **succeeds** because it demonstrates dedup + live fan-out + replay
+ refcounted cancel + backpressure + socket auth on a real fork-per-connection
daemon, so the team can answer RFC Q0/Q1 with evidence and **freeze the Phase 3
internal coordinator interface (§3)** — leaving only the public-wire field set
(Q4, with Hydra) outstanding. It would have **failed** had the coordinator been
unable to preserve build isolation, enforce trust before subscribe, or keep the
build decoupled from slow clients — none of which occurred.
