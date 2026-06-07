# Validation & freeze-readiness plan (Tier 2)

| | |
|------------------|------------------------------------------------|
| **Status**       | Prototypes landed — engineering validation discharged for A/B/C/D; remaining gates are external (Hydra) + a release-cycle soak |
| **Parent**       | [`remote-build-protocol-redesign.md`](./remote-build-protocol-redesign.md) |
| **Decisions**    | [`*.decisions.md`](./remote-build-protocol-redesign.decisions.md) (Blockers 1–3, O1–O7) |
| **Spike**        | [`*.spike.md`](./remote-build-protocol-redesign.spike.md) (§4 prototype) |
| **Prototypes**   | [`prototypes/workstream-a-coordinator/`](./prototypes/workstream-a-coordinator/) (A+B+C) · [`prototypes/workstream-d-serve-diag/`](./prototypes/workstream-d-serve-diag/) (D) |

> Every *design* question is decided (Blockers 1–3, O1–O7). What remains is
> **execution, not clarification**: prototypes, tests, and one external
> coordination. This document does **not** re-decide anything — it pulls the
> owed validation work out of the decisions record's "Follow-up" sections, the
> spike's §4 prototype plan, and RFC §9, into one **sequenced, checklist-driven**
> plan so each item has a concrete deliverable, acceptance criteria, file touch
> points, and an owner. It is the answer to "how do we *finish* Tier 2."

> **Prototype status (2026-06).** The throw-away validation prototypes the plan
> calls for now exist and **all their suites pass** (`make check` / `check-b` /
> `check-c` for A/B/C; `make check` for D). That discharges the *engineering*
> validation behind every workstream and lets us **resolve the freezes
> accordingly** — but only as far as code can: see the verdict below and the
> checked boxes in the readiness section.
>
> | Freeze | Verdict | What still gates it |
> |---|---|---|
> | **F-INT** | ✅ **Freezable now** — no remaining gate | — (the §3 internal coordinator interface is fully validated by Workstream A; productionizing it in `libstore`/`daemon` is Phase 3 *implementation*, not a freeze gate) |
> | **F-WIRE** | ⏳ **Nix-side cleared**, freeze still blocked | only the **serve half** of the Build Session additions needs Hydra (gate **H3**, the deferred serve set — a *later follow-on* of the same coordination as the diagnostic core, opened once Phase 3 dedup semantics settle; the worker-protocol ops are Nix-internal). T1–T3 (B) and C-a…C-f (C) are green. See [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks). |
> | **F-SERVE-DIAG** | ⏳ **Layout + back-compat proven**, freeze still blocked | **D3.1** named-maintainer sign-off, **D3.2** queue-runner branch, **D3.4** ≥1-cycle soak — all external/time-gated; D1/D2/D3.3 proven in-prototype, and **D3.3 now also ported into `src/libstore-tests`** (`serve-diag-core.cc`, `SERVE_PROTOCOL_VERSION` unbumped). **Compatibility correction:** ship as serve **2.9** (minor bump within major 2), *not* `{3,0}` — a major bump is rejected by deployed clients at handshake before `min()` (decisions Blocker 3). |
>
> The honest one-liner: **F-INT resolves now; F-WIRE and F-SERVE-DIAG have their
> engineering preconditions discharged but cannot freeze until Hydra coordinates
> and (for serve diagnostic core) the field set soaks** — exactly the external long pole D4
> exists to start.

## 0. The three freezes this plan gates (and why they are distinct)

Conflating these is the main reason Tier 2 reads as vague. They gate on
different evidence and can happen at different times:

| Freeze | What it locks | Public wire? | Gated by |
|---|---|---|---|
| **F-INT** — Phase 3 *internal* coordinator interface (spike §3) | the child↔coordinator control protocol, registry, replay/refcount semantics | **No** (machine-internal) | Workstream **A** |
| **F-WIRE** — Phase 3 *public* Build Session surface | the worker/serve additions for attach / `deduplicated` / `QueryActiveBuilds` | **Yes** (additive, version-gated) | Workstreams **B** + **C** proven (the worker-protocol ops are Nix-internal); its *serve-side* fields additionally need Hydra gate **H3** (deferred serve set, same coordination as the diagnostic core) — see [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks) |
| **F-SERVE-DIAG** — serve diagnostic core (Blocker 3) | `logRef`, `failurePhase`/`exitCode`/`logTail`, `QueryBuildLog`; bump `SERVE_PROTOCOL_VERSION` to `(2<<8\|9)` | **Yes** | Workstream **D** (the four Blocker-3 criteria) |

Key independence: **F-SERVE-DIAG does not need the coordinator.** The serve
diagnostic core (Phases 0–1) is orthogonal to dedup/attach, so Workstream D can
run fully in parallel with A/B/C. The operational fixes (Gaps A/B/C, Phases
0–2/4) gate on **nothing here** and ship first.

---

## The external gates (Hydra) — what is actually owed, by whom, and which freeze each blocks

Every other gate in this plan is engineering this repo can discharge alone —
and has: the A/B/C/D prototypes are green and the back-compat goldens are ported
into `src/libstore-tests`. What remains are the **external** gates: the ones that
need the *Hydra* project, not this repo. They are the long pole, so this section
names them precisely — because the two places the plan says "Hydra field set"
(the **F-WIRE** row and the **F-SERVE-DIAG** criteria) read like two separate
asks when they are not.

**The one fact that disambiguates everything:** `hydra-queue-runner` speaks only
the **serve** protocol — the stripped-down `BasicClientConnection` /
`BasicServerConnection` shared for exactly that purpose
(`serve-protocol.hh:96-101`, §2.1) — and **never the worker protocol** (§2.2).
Two consequences fall straight out:

- **Nothing on the worker protocol is a Hydra gate.** The Phase-3 Build Session
  ops (`attach` / `deduplicated` / `QueryActiveBuilds`) also exist on the worker
  protocol behind its own version bump (§7), but Hydra never reads those bytes.
  Their freeze is gated by the Nix-side trust/cancel proofs (Workstreams **B**+**C**),
  not by Hydra.
- **Only the *serve* surface ever needs Hydra**, and there the new fields split
  exactly as [Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51)
  splits them: the **frozen diagnostic core** (`logRef`,
  `failurePhase`/`exitCode`/`logTail`, `QueryBuildLog`) versus the **deferred
  set** (`builderId`, serve `deduplicated`, the §4.4 classification fields) that
  stays behind the *unstable* serve version until Phase 3 semantics settle.

From that, there are exactly **three human-external gates** (`H1`–`H3`) and **one
clock**:

| # | External gate | Blocks | What Hydra must actually do | "Done" means | Owner |
|---|---|---|---|---|---|
| **H1** | Serve diagnostic-core sign-off (= **D3.1**) | **F-SERVE-DIAG** | a *named* queue-runner maintainer reviews and signs off the exact frozen field set **and byte order** | a name recorded in the decisions record against the layout in the coordination post | Hydra queue-runner maintainer |
| **H2** | Queue-runner consumer branch (= **D3.2**) | **F-SERVE-DIAG** | a `hydra-queue-runner` branch consumes `QueryBuildLog` + the structured log frames to **drop its out-of-band log capture**, and reads the extended `BuildResult` | the branch builds and validates against a new-Nix (2.9) builder | Hydra queue-runner maintainer |
| **H3** | Deferred serve-set agreement | **F-WIRE** (serve half only) | *once Phase 3 settles* `deduplicated`/`builderId` semantics, agree their serve byte layout on the **same** serve channel as H1 — a later follow-on, **not** a separate thread | same sign-off shape as H1, on the deferred fields | Hydra queue-runner maintainer |

Plus **D3.4** — the ≥1-release-cycle **soak** of the diagnostic-core layout on the
unstable serve version with no change. It is no one's *action*: it is a clock that
can only start once H1's layout is agreed, and the ported goldens
(`serve-diag-core.cc`, with its `static_assert` pinning
`SERVE_PROTOCOL_VERSION == (2<<8|8)`) are what guard the layout *during* that soak.

**The relationships that the two scattered "Hydra field set" mentions obscure:**

- **F-SERVE-DIAG's entire external block is H1 + H2 (+ the D3.4 clock).** Its
  engineering criterion D3.3 (golden back-compat, both directions) is already
  discharged in-repo, so H1/H2 are the only human-external items and D3.4 the
  only wait. Nothing here is a coding task this repo can finish.
- **H3 is F-WIRE's *only* external dependency, and it is the *same* coordination
  as H1, deferred — not a second Hydra ask.** F-WIRE's worker-protocol ops are
  Nix-internal (B+C). Its serve-side fields **are** the deferred set, which
  Blocker 3 forbids freezing until Phase 3 dedup semantics settle — so H3 cannot
  even *open* before then, and when it does it rides the serve channel H1 already
  established. There is **one** Hydra coordination effort — the thread drafted in
  [`*.hydra-coordination.md`](./remote-build-protocol-redesign.hydra-coordination.md) —
  carrying H1/H2 now and H3 as a later field-set follow-on on the same wire.
- **All three are version-gated in both directions** (`min(client,server)` over
  the major-2 floor of Blocker 3's compatibility correction), so none is a flag
  day: a Hydra that has neither signed off nor built anything keeps working
  byte-for-byte at ≤2.8 the entire time.

**Sequencing of the external work:** open the coordination thread (**D4**) first —
it is the slowest, external long pole — and drive **H1 → H2** (with the **D3.4**
soak) to bump serve to `(2<<8|9)`; **H3** follows whenever Phase 3's dedup
semantics freeze. Until H1/H2 land *and* the soak elapses, `SERVE_PROTOCOL_VERSION`
stays at `(2<<8|8)`.

---

## Workstream A — Cross-process coordination prototype  → gates **F-INT**

Consolidates spike §4 and is the validation vehicle for **O1–O5** as well as the
core of Blocker 1/2. Throw-away branch; no merged production C++.

**Deliverables**

- **A1 — coordinator binary** (`nix-daemon --coordinator` role, per
  [O1](./remote-build-protocol-redesign.decisions.md#operational-decision-o1--coordinator-deployment-model-spike-6-q1)):
  `START_OR_ATTACH`/`SUBSCRIBE`/`UNSUBSCRIBE`/`CANCEL_HINT`/`QUERY_ACTIVE`
  (spike §3.2); registry; in-memory replay buffer with the
  [O5](./remote-build-protocol-redesign.decisions.md#operational-decision-o5--replay-cap-default-and-truncation-ux-rfc-q1-remainder-spike-6-q4)
  cap (4 MiB, head+tail+marker); refcount; runs the build via the existing
  `Worker`/`DerivationBuildingGoal` path so output `PathLocks` still apply.
- **A2 — feature-flagged relay path** in the daemon child: on a
  capability-negotiated request, `START_OR_ATTACH` + relay `FRAME`s to the client
  socket instead of building locally; rewire `MonitorFdHup` →`UNSUBSCRIBE`
  (spike §3.4). Includes the
  [O2](./remote-build-protocol-redesign.decisions.md#operational-decision-o2--coordinator-crash-recovery-posture-spike-6-q5)
  EOF→build-locally fallback and the
  [O3](./remote-build-protocol-redesign.decisions.md#operational-decision-o3--lazy-spawn-lifecycle-spike-6-q8)
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

> **Prototype: ✅ done.** [`prototypes/workstream-a-coordinator/`](./prototypes/workstream-a-coordinator/)
> (`coordinator.cc`/`reldaemon.cc`/`slow-builder.sh` = A1/A2/A3). `make check`
> runs all criteria green on a real `fork()`-per-connection daemon model:
> A-dedup, A-replay, A-backpressure, A-sockauth (real `SO_PEERCRED`), A-crash
> (real `PR_SET_PDEATHSIG`), A-spawn all PASS; A-throughput **measured**
> (single-threaded coordinator ≈0.2 CPU-s across 64 builds × 4 subscribers —
> well under any ceiling that would force the O4 sharding; exact CPU figure is
> host-dependent). **F-INT is freezable.**

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

> **Prototype: ✅ done.** Extends the A1 coordinator (`promote()`/
> `checkPromotions()`, spike §3.8). `make check-b` is green: **T1** (existence
> oracle — byte-identical denials, |Δmedian| ≈ 11 µs ≪ 500 µs threshold,
> authorize-before-registry), **T2** (CA-merge re-auth — unauthorized peer
> detached at promotion, observes none of the authorized peer's log, one build
> on the resolved key), **T3** (asserted-key spoof rejected; coordinator
> recomputes the key), plus the `b-resolve` happy-path race. The F-WIRE
> *precondition* is met; the freeze itself still waits on the RFC §7 Hydra
> field set.

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

> **Prototype: ✅ done.** `C1` lives in the same coordinator
> (`hasRootReasonToContinue()` = explicit-root-only; `checkDeadlines()` /
> `maybeFinalize()` / `onUnsubscribe()`). `make check-c` is green across the
> full matrix C-a…C-f (no originator privilege; per-subscriber `TimedOut`
> detach under the max envelope; `--keep-failed` logical OR; scoped cancel).
> No wire change, so no freeze gated — but the table is now proven ahead of
> Phase 3 implementation.

---

## Workstream D — serve diagnostic core + Hydra coordination  → gates **F-SERVE-DIAG**  [Blocker 3]

Independent of A/B/C. D1/D2 can land now behind the **unstable** version; D3/D4
are the gate to bumping `SERVE_PROTOCOL_VERSION`.

**Deliverables**

- **D1 — diagnostic core behind the unstable version:** `logRef`,
  `failurePhase`/`exitCode`/`logTail` appended after the 2.8 `builtOutputs` block
  under a `>= {2,9}` guard (binary, not JSON); `QueryBuildLog` as `Command = 10`
  (decisions B3 §3). `builderId`/`deduplicated` stay deferred/unstable.
- **D2 — golden/characterisation tests** (decisions B3 §4): round-trip
  `BuildResult` at 2.3/2.6/2.8/2.9; an explicit **2.8-reads-2.9-bytes** test
  (2.8 reader consumes exactly the 2.8 fields; a negotiated-down peer never emits
  the 2.9 tail); a `QueryBuildLog` round-trip; a functional test that `nix log`
  over the serve path returns the real log (Gap A/§4.5).

**The four freeze criteria** (Blocker 3 — *all* must hold to bump to 2.9):

- [ ] **D3.1** — a **named** Hydra queue-runner maintainer has reviewed and
  signed off on the exact field set and byte order. *(Naming them is the first
  action and the single biggest external dependency in the whole set.)*
- [ ] **D3.2** — a `hydra-queue-runner` branch (a) consumes `QueryBuildLog` + the
  structured log frames to **drop its out-of-band log capture**, and (b) reads
  the extended `BuildResult`, both validated against a new-Nix builder.
- [ ] **D3.3** — D2's golden tests prove round-trip at 2.8 and 2.9 **and** that a
  2.8 peer ignores 2.9 fields (full back-compat matrix, both directions).
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

> **Prototype: ✅ D1/D2 done; D3.3 proven standalone.**
> [`prototypes/workstream-d-serve-diag/`](./prototypes/workstream-d-serve-diag/)
> mirrors the `serve-protocol.cc` version-gated ladder and appends the frozen
> diagnostic core under a `>= {2,9}` guard. `make check` is green across 21
> golden/characterisation tests: round-trip at 2.3/2.6/2.8/2.9, exact golden
> bytes, additive layout, **2.8-reads-2.9-bytes**, negotiated-down emits no
> tail, the back-compat matrix **both directions**, `QueryBuildLog` round-trip
> (`nix log` over serve), and the deferred `builderId`/`deduplicated` staying
> behind the unstable gate. This *is* D3.3's proof. **The literal checklist item
> is now closed: the goldens are ported into `src/libstore-tests`** as
> `serve-diag-core.cc` (9 gtest cases mirroring the prototype's ladder), a
> self-contained characterisation of the candidate 2.9 layout that pins
> `SERVE_PROTOCOL_VERSION == (2<<8|8)` via `static_assert` so it guards the
> layout in CI during the soak without touching the production serializer or
> bumping the wire. **D3.1, D3.2, and D3.4 are external/time gates no prototype
> can close** — they are why D4 (the Hydra thread) is the long pole.
> `SERVE_PROTOCOL_VERSION` stays unbumped.

---

## Sequencing (what unblocks what)

```
Phases 0–2/4 (Gaps A/B/C) ────────────────────────────────►  ship now, gate on nothing here

Workstream A ──► F-INT (freeze internal coordinator iface)
      └─► Workstream B (T1–T3) ─┐
      └─► Workstream C (C1+matrix) ─┴─► (+ serve-side gate H3, deferred) ──► F-WIRE

Workstream D:  D1+D2 (now, behind unstable) ──► D3.1…D3.4 + D4 ──► F-SERVE-DIAG
                                                 (D is parallel to A/B/C)
```

- **A is the trunk** for dedup/attach: B and C extend its prototype.
- **B and C** are the F-WIRE preconditions on the Nix side; its only external
  dependency is the serve-side **H3** (the deferred serve set), which is a later
  follow-on of the *same* Hydra coordination as the diagnostic core, not a
  separate ask — see [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).
- **D is parallel and independent** — start D4 (Hydra thread) earliest because it
  is the slowest, external dependency, and it carries H1/H2 now and H3 later.

## Readiness checklists (copy-paste gates)

**Freeze F-INT (Phase 3 internal interface):** ✅ **all engineering gates met —
assessed freezable; awaiting the maintainer's freeze decision.**
- [x] A1, A2, A3 built; A-dedup, A-replay, A-backpressure, A-sockauth, A-crash,
  A-spawn all green; A-throughput measured and within the provisional ceiling
  (no escalation needed). *(Workstream A prototype, `make check`.)*
- [ ] **Freeze sign-off by the libstore/daemon maintainer.** This is the one
  remaining F-INT action and it is **not** a coding task: the evidence above
  discharges every *engineering* precondition (the §3 child↔coordinator control
  protocol, registry, replay/refcount semantics are all validated), so this
  record **assesses F-INT freezable** — but the actual decision to freeze the
  internal interface belongs to the libstore/daemon maintainer, not to this
  validation pass. Nothing further is owed from a coding session.

**Freeze F-WIRE (Phase 3 public Build Session surface):** ⏳ Nix-side cleared;
blocked only on the serve-side **H3** (the deferred serve set) — a later follow-on
of the diagnostic-core coordination, not a separate Hydra ask.
- [x] F-INT done.
- [x] **T1, T2, T3** green (Workstream B prototype, `make check-b`).
- [x] **C1 + the `build-dedup-cancel` matrix (C-a…C-f)** green (Workstream C
  prototype, `make check-c`).
- [ ] **H3** — the *serve-side* fields of the Build Session surface (the deferred
  set: serve `deduplicated`/`builderId`) agreed with Hydra. **External, and the
  only remaining F-WIRE gate.** It is a *later follow-on* of the diagnostic-core
  coordination (H1) on the same serve channel, and cannot open until Phase 3
  dedup semantics settle (Blocker 3 forbids freezing them sooner). The
  worker-protocol ops carry no Hydra gate. See [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).

**Bump `SERVE_PROTOCOL_VERSION` → `(2<<8|9)` (F-SERVE-DIAG):** ⏳ layout + back-compat
proven; blocked on external sign-off + soak.
- [x] D1 implemented behind unstable (modelled); D2 golden/characterisation
  tests green (Workstream D prototype, `make check`).
- [x] **D3.3** (golden back-compat both ways) — **ported into `src/libstore-tests`**
  as `serve-diag-core.cc` (9 gtest cases: round-trip at 2.3/2.6/2.8/2.9, golden
  bytes, additive layout, 2.8-reads-2.9-bytes, negotiated-down emits no tail, the
  back-compat matrix both directions incl. the `{3,0}`-rejection regression
  guard, and a `QueryBuildLog` round-trip). The port is a self-contained
  characterisation of the **candidate** 2.9 layout: it does **not** touch the
  production serve serializer and a `static_assert` pins
  `SERVE_PROTOCOL_VERSION == (2<<8|8)`, so it guards the layout during the soak
  without bumping the wire. At freeze, the goldens retarget the real serializer
  and the self-contained model is deleted. *(The literal checklist item is now
  closed; D3.1/D3.2/D3.4 below still gate the freeze.)*
- [ ] **D3.1 / gate H1** (named maintainer sign-off on the field set + byte order) — external.
- [ ] **D3.2 / gate H2** (queue-runner branch consuming `QueryBuildLog` + the
  structured frames, against a 2.9 builder) — external.
- [ ] **D3.4** (≥1-cycle soak on the unstable version) — time-gated; the clock
  only starts once H1 fixes the layout. *(H1/H2/D3.4 are the whole external block;
  see [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).)*

## Forward-compatibility & elastic-backend additions (gate no freeze)

The RFC's elastic/distributed additions — the distributed-backend class
(§4.3.4), the persistence/high-load seams (§4.3.5), builder-internal failure
classification + right-sized retry (§4.4, §4.7.5), multi-architecture pools
(§4.7), the deployment/operational model (§4.10), and the forward-compatibility
guardrails (§8.1) — are **all below the client wire** and therefore **gate none
of the three freezes** above. They are tracked here so the plan stays complete:

- **§8.1 guardrails → a Phase 3 implementation-review gate (not a freeze gate).**
  Each PR implementing Phases 0–7 is checked against the eight guardrails (key on
  the resolved drv; program against the registry interface; refcounted cancel
  even when count = 1; keep the elastic opt-out a real branch; per-build activity
  tagging from the start; no build/connection host-or-store locality assumption;
  append-only deferred wire fields; no un-shardable global state). A violation is
  a review blocker because each is cheap now and expensive to retrofit.
- **New functional tests (added to the inventory; gate no freeze):**

  | ID | Scenario | Assert |
  |---|---|---|
  | **R-class** | a builder-internal failure (e.g. OOM / exit 137 under a memory cap) vs. a build-intrinsic failure | the `BuildResult` failure carries a **transient/retryable** class + (for resource exhaustion) a resource hint, *distinct* from a build-error class; a transient failure is **not** cached/reused — no key poisoning (RFC §4.4) |
  | **M-arch** | concurrent builds of the same package for `x86_64-linux` and `aarch64-linux` against one endpoint | **two distinct keys, never coalesced**; each result reused only for its own system (RFC §4.7; guardrail §8.1 #1) |

  > **Placement decided: throw-away prototype, not real tests.** Both exercise
  > Phase-3 *coordinator* semantics (failure classification + retry sizing; the
  > build-key definition) that have no production code yet, and the §4.4
  > classification fields ride the **deferred** serve set — so a `tests/functional`
  > or `src/libstore-tests` test would have nothing real to drive. They live in
  > the Workstream-A coordinator prototype (`make check-fwd`):
  > **R-class** — `tests/r-class.sh` (10 assertions: OOM/exit-137 → `class=transient`
  > + `hint=memory`, distinct from a build-error class, and a re-run of the same
  > key starts a fresh build — proving the failure is not cached / no key
  > poisoning); **M-arch** — `tests/m-arch.sh` (9 assertions: a package built for
  > `x86_64-linux` vs `aarch64-linux` yields two distinct keys → two builds, never
  > coalesced, with an identical-key positive control that *does* coalesce). Both
  > green. Modelling the deferred failure-classification fields required a small
  > extension to the prototype's client-facing result record (a `FailClass` +
  > `resourceHint`), kept off the frozen 2.9 core exactly as Blocker 3 specifies.

- The §4.4 failure-classification fields ride the **deferred** serve set
  (decisions Blocker 3), so they do **not** move the F-SERVE-DIAG gate.

## Owners at a glance

| Workstream | Gate | Owner |
|---|---|---|
| A — coordination prototype | F-INT | libstore/daemon |
| B — CA-merge trust tests | F-WIRE precond. | security reviewer + libstore/protocol |
| C — refcounted cancel | (no wire) pre-Phase-3 impl | libstore/protocol |
| D — serve diagnostic core + Hydra | F-SERVE-DIAG | Hydra queue-runner maint. (TBD) + libstore/serve-protocol |
