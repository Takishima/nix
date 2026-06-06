# Workstream A prototype — cross-process build coordination

> **Status:** throw-away validation prototype.
> **Gates:** freeze **F-INT** (the Phase 3 *internal* coordinator interface).
> **Parent:** [`../../remote-build-protocol-redesign.validation.md`](../../remote-build-protocol-redesign.validation.md)
> (Workstream A) · [`../../remote-build-protocol-redesign.spike.md`](../../remote-build-protocol-redesign.spike.md)
> (§3 interface, §4 prototype plan)

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
| `client.cc` | `nix build` stand-in (normal / slow-reader / disconnect behaviors) | test driver |
| `ctl.cc` | direct control-socket probe (sockauth + `QUERY_ACTIVE`) | test driver |
| `tests/` | the acceptance-criteria harness | — |

## Build & run

```sh
make            # builds coordinator, reldaemon, client, ctl  (needs g++/clang++, C++20, Linux)
make check      # builds + runs the full acceptance-criteria harness
WSA_DEBUG=1 ./reldaemon /tmp/wsa &        # manual: start a daemon
./client --state /tmp/wsa -k demo -t 1 -L # manual: build something
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

Refcounted cancel (the core of Workstream C's matrix that this prototype owns) is
folded into `a-cancel.sh`: **C-a** (originator drops, build continues for the
other subscriber), **C-b** (sole client drops, no root → cancel), **C-c** (sole
client drops but holds an explicit root → build completes), per §3.4 / §5.2.

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

**Out of scope** (spike §4.3), intentionally absent: CA `resolving` pre-state
(Workstream B; this targets the input-addressed path), session re-attach after a
fully dropped connection (Phase 6), full crash/restart re-adoption (only the
degrade-to-PathLocks claim is demonstrated), `QueryActiveBuilds` UX, any
serve-protocol bridge, and **wire freezing** (this *informs* Phase 3, it does not
freeze it).

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
