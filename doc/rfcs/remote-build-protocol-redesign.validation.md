# Validation & freeze-readiness plan (Tier 2)

| | |
|------------------|------------------------------------------------|
| **Status**       | Execution plan — consolidates the "decided but not yet validated" work |
| **Parent**       | [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md) |
| **Decisions**    | [`*.decisions.md`](./remote-build-protocol-redesign.decisions.md) (Blockers 1–3, O1–O7) |
| **Spike**        | [`*.spike.md`](./remote-build-protocol-redesign.spike.md) (§4 prototype) |
| **Tracks**       | open-points [§2](./remote-build-protocol-redesign.open-points.md) (validation debt) |

> Every *design* question is decided (Blockers 1–3, O1–O7). What remains is
> **execution, not clarification**: prototypes, tests, and one external
> coordination. This document does **not** re-decide anything — it pulls the
> owed validation work out of the decisions record's "Follow-up" sections, the
> spike's §4 prototype plan, and RFC §9, into one **sequenced, checklist-driven**
> plan so each item has a concrete deliverable, acceptance criteria, file touch
> points, and an owner. It is the answer to "how do we *finish* Tier 2."

## 0. The three freezes this plan gates (and why they are distinct)

Conflating these is the main reason Tier 2 reads as vague. They gate on
different evidence and can happen at different times:

| Freeze | What it locks | Public wire? | Gated by |
|---|---|---|---|
| **F-INT** — Phase 3 *internal* coordinator interface (spike §3) | the child↔coordinator control protocol, registry, replay/refcount semantics | **No** (machine-internal) | Workstream **A** |
| **F-WIRE** — Phase 3 *public* Build Session surface | the worker/serve additions for attach / `deduplicated` / `QueryActiveBuilds` | **Yes** (additive, version-gated) | Workstreams **B** + **C** proven, *and* RFC §7/Q4 (Hydra field set) |
| **F-SERVE30** — serve 3.0 diagnostic core (Blocker 3) | `logRef`, `failurePhase`/`exitCode`/`logTail`, `QueryBuildLog`; bump `SERVE_PROTOCOL_VERSION` to `(3<<8\|0)` | **Yes** | Workstream **D** (the four Blocker-3 criteria) |

Key independence: **F-SERVE30 does not need the coordinator.** The serve
diagnostic core (Phases 0–1) is orthogonal to dedup/attach, so Workstream D can
run fully in parallel with A/B/C. The operational fixes (Gaps A/B/C, Phases
0–2/4) gate on **nothing here** and ship first.

---

## Workstream A — Cross-process coordination prototype  → gates **F-INT**

Consolidates spike §4 and is the validation vehicle for **O1–O5** as well as the
core of Blocker 1/2. Throw-away branch; no merged production C++.

**Deliverables**

- **A1 — coordinator binary** (`nix-daemon --coordinator` role, per
  [O1](./remote-build-protocol-redesign.decisions.md#operational-decision-o1--coordinator-deployment-model-spike-6-q1-open-points-11)):
  `START_OR_ATTACH`/`SUBSCRIBE`/`UNSUBSCRIBE`/`CANCEL_HINT`/`QUERY_ACTIVE`
  (spike §3.2); registry; in-memory replay buffer with the
  [O5](./remote-build-protocol-redesign.decisions.md#operational-decision-o5--replay-cap-default-and-truncation-ux-rfc-q1-remainder-spike-6-q4-open-points-17)
  cap (4 MiB, head+tail+marker); refcount; runs the build via the existing
  `Worker`/`DerivationBuildingGoal` path so output `PathLocks` still apply.
- **A2 — feature-flagged relay path** in the daemon child: on a
  capability-negotiated request, `START_OR_ATTACH` + relay `FRAME`s to the client
  socket instead of building locally; rewire `MonitorFdHup` →`UNSUBSCRIBE`
  (spike §3.4). Includes the
  [O2](./remote-build-protocol-redesign.decisions.md#operational-decision-o2--coordinator-crash-recovery-posture-spike-6-q5-open-points-12)
  EOF→build-locally fallback and the
  [O3](./remote-build-protocol-redesign.decisions.md#operational-decision-o3--lazy-spawn-lifecycle-spike-6-q8-open-points-14)
  spawn/election.
- **A3 — slow test derivation** emitting marker lines + a counter file so a
  second client reliably attaches mid-build.

**Acceptance criteria** (extends spike §4.2 — all must hold on the *stock
fork-per-connection daemon*):

| ID | Assertion | Proves |
|---|---|---|
| A-dedup | two concurrent clients on the same drv → `builder_counter == 1` | exactly one build runs |
| A-replay | client2's `replayed=true` prefix + live tail == client1's full stream, no gap/dup at the seam | O5 replay correctness |
| A-backpressure | a client that stops reading does **not** stall the builder (counter keeps advancing); it is demoted | spike §3.6 |
| A-sockauth | a process connecting to the control socket as a *different uid* is refused at the peer-cred check before any `sessionAuth` is read | O1 / spike §3.7.1 |
| A-crash | kill the coordinator mid-build → no orphan builder survives; clients fall back to local build; **exactly one** rebuild; no partial output ever seen valid; daemon respawns a coordinator for the next build | O2 |
| A-spawn | N children racing first-requests → exactly one coordinator + one socket; idle-exit-then-connect respawns; stale socket reclaimed | O3 |
| A-throughput | record coordinator CPU + per-frame fan-out under ≥64 parallel builds × several subscribers (measurement, not pass/fail) | O4 ceiling data |

**Owner:** libstore/daemon maintainer. **Out of scope (spike §4.3):** CA
`resolving` (→ Workstream B), session re-attach, full crash-recovery
productionization, serve bridge.

---

## Workstream B — Trust validation under CA key-merge  → gates **F-WIRE**  [Blocker 1]

The **one remaining unproven mechanism** (decisions B1 §5): the CA
`resolving`→promote path and re-authorize-on-promotion. Extends the A1 prototype
(which targets input-addressed first).

**Deliverables**

- **B1 — CA `resolving`→promote** in the prototype (spike §3.8): provisional
  entry keyed on the unresolved drv; promote/merge onto the resolved key;
  **re-authorize every subscriber against the resolved key at promotion**, detach
  failures with an error.

**Tests** (the three trust tests of decisions B1 §4 — these are the literal
**F-WIRE precondition**):

| ID | Scenario | Assert |
|---|---|---|
| **T1 — existence oracle** | unauthorized client issues `START_OR_ATTACH`/`QUERY_ACTIVE` for a resolved key that *is* in flight, and for one that is *not* | byte-identical denials, **no measurable timing difference**; learns neither log, drv name, nor existence |
| **T2 — CA-merge re-auth** | two distinct unresolved drvs (A authorized, B not for the *resolved* key) resolving to the same key | B detached with an error at promotion; B observes none of A's log/inputs; shared `Build` exposes only resolved-drv state |
| **T3 — asserted-key spoof** | a child presents a `buildKey` not matching `drvForBuild` | rejected; coordinator recomputes the key from the resolved drv it received |

**Owner:** security reviewer + libstore/protocol (joint). **Gate:** T1–T3 green
**before any F-WIRE freeze.**

---

## Workstream C — Refcounted-cancel implementation  → coordinator-internal (no wire)  [Blocker 2]

Changes **no wire** (decisions B2 §5), so it does not gate a freeze — but its
table must be implemented and tested before Phase 3 *implementation* is trusted.

**Deliverables**

- **C1 —** implement `hasRootReasonToContinue()` as **explicit-root-only**, and
  the **per-subscriber deadline + max-envelope** timer (the spike §5.2 stubs both
  to `false`).

**Test — `build-dedup-cancel` matrix** (the six cases of decisions B2 §4):

| ID | Case | Assert |
|---|---|---|
| C-a | two attached, kill #1 (originator) | build continues; #2 completes |
| C-b | one attached, disconnect, no root | build cancels (counter ≠ completion) |
| C-c | one attached **with explicit root**, disconnect | build completes |
| C-d | two timeouts 10 min / 60 min | at 10 min #1 gets `TimedOut` + detaches; build continues for #2 |
| C-e | fail with `--keep-failed` set by exactly one of two | failed dir preserved (**OR**) |
| C-f | active-cancel by one of two | canceller gets local interrupt; other completes |

**Owner:** libstore/protocol (build scheduling / `Worker` lifetime).

---

## Workstream D — serve 3.0 diagnostic core + Hydra coordination  → gates **F-SERVE30**  [Blocker 3]

Independent of A/B/C. D1/D2 can land now behind the **unstable** version; D3/D4
are the gate to bumping `SERVE_PROTOCOL_VERSION`.

**Deliverables**

- **D1 — diagnostic core behind the unstable version:** `logRef`,
  `failurePhase`/`exitCode`/`logTail` appended after the 2.8 `builtOutputs` block
  under a `>= {3,0}` guard (binary, not JSON); `QueryBuildLog` as `Command = 10`
  (decisions B3 §3). `builderId`/`deduplicated` stay deferred/unstable.
- **D2 — golden/characterisation tests** (decisions B3 §4): round-trip
  `BuildResult` at 2.3/2.6/2.8/3.0; an explicit **2.8-reads-3.0-bytes** test
  (2.8 reader consumes exactly the 2.8 fields; a negotiated-down peer never emits
  the 3.0 tail); a `QueryBuildLog` round-trip; a functional test that `nix log`
  over the serve path returns the real log (Gap A/§4.5).

**The four freeze criteria** (Blocker 3 — *all* must hold to bump to 3.0):

- [ ] **D3.1** — a **named** Hydra queue-runner maintainer has reviewed and
  signed off on the exact field set and byte order. *(This is open-points §3.3;
  naming them is the first action.)*
- [ ] **D3.2** — a `hydra-queue-runner` branch (a) consumes `QueryBuildLog` + the
  structured log frames to **drop its out-of-band log capture**, and (b) reads
  the extended `BuildResult`, both validated against a new-Nix builder.
- [ ] **D3.3** — D2's golden tests prove round-trip at 2.8 and 3.0 **and** that a
  2.8 peer ignores 3.0 fields (full back-compat matrix, both directions).
- [ ] **D3.4** — the field set has soaked on the **unstable version for ≥1
  release cycle** with no layout change.

- **D4 — open the Hydra coordination thread** (RFC Q4 puts the coordination
  itself out of scope of the RFC). This is the **external long pole** — start it
  early; D3.1/D3.2 cannot complete without it. A ready-to-post **opening post is
  drafted** at
  [`*.hydra-coordination.md`](./remote-build-protocol-redesign.hydra-coordination.md)
  (the ask, the frozen-candidate field set + byte order, the back-compat matrix,
  and the specific questions for Hydra maintainers).

**Owner:** Hydra queue-runner maintainer (sign-off) + libstore/serve-protocol
maintainer (serializer + version bump).

---

## Sequencing (what unblocks what)

```
Phases 0–2/4 (Gaps A/B/C) ────────────────────────────────►  ship now, gate on nothing here

Workstream A ──► F-INT (freeze internal coordinator iface)
      └─► Workstream B (T1–T3) ─┐
      └─► Workstream C (C1+matrix) ─┴─► (with RFC §7 Hydra field set) ──► F-WIRE

Workstream D:  D1+D2 (now, behind unstable) ──► D3.1…D3.4 + D4 ──► F-SERVE30
                                                 (D is parallel to A/B/C)
```

- **A is the trunk** for dedup/attach: B and C extend its prototype.
- **B and C** are the F-WIRE preconditions on the Nix side; the Hydra field set
  (RFC §7) is the third.
- **D is parallel and independent** — start D4 (Hydra thread) earliest because it
  is the slowest, external dependency.

## Readiness checklists (copy-paste gates)

**Freeze F-INT (Phase 3 internal interface):**
- [ ] A1, A2, A3 built; A-dedup, A-replay, A-backpressure, A-sockauth, A-crash,
  A-spawn all green; A-throughput measured and within (or escalated against) the
  provisional ceiling.

**Freeze F-WIRE (Phase 3 public Build Session surface):**
- [ ] F-INT done; **T1, T2, T3** green (Workstream B); **C1 + the
  `build-dedup-cancel` matrix (C-a…C-f)** green (Workstream C); Hydra field set
  for the public Build Session additions agreed (RFC §7).

**Bump `SERVE_PROTOCOL_VERSION` → `(3<<8|0)` (F-SERVE30):**
- [ ] D1 implemented behind unstable; D2 golden/characterisation tests green;
  **D3.1 (named maintainer sign-off), D3.2 (queue-runner branch), D3.3 (golden
  back-compat both ways), D3.4 (≥1-cycle soak)** all checked.

## Owners at a glance

| Workstream | Gate | Owner |
|---|---|---|
| A — coordination prototype | F-INT | libstore/daemon |
| B — CA-merge trust tests | F-WIRE precond. | security reviewer + libstore/protocol |
| C — refcounted cancel | (no wire) pre-Phase-3 impl | libstore/protocol |
| D — serve 3.0 + Hydra | F-SERVE30 | Hydra queue-runner maint. (TBD) + libstore/serve-protocol |
