# Validation & freeze-readiness plan (Tier 2)

| | |
|------------------|------------------------------------------------|
| **Status**       | Prototypes landed — engineering validation discharged for A/B/C/D; the serve diagnostic core freezes on Nix-side criteria + a release-cycle soak (Hydra review solicited, not blocking, revised 2026-06); no hard external-Hydra freeze blocker remains — the deferred serve set (F-WIRE/H3) is gated on Phase 3 design maturing (internal), not on Hydra |
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
> | **F-WIRE** | ⏳ **Nix-side cleared**, serve half deferred | the worker-protocol ops are Nix-internal (B+C, green); the **serve-side** fields (`deduplicated`/`builderId`, gate **H3**) freeze later — gated on the Phase 3 coordinator/dedup design settling (**design-maturity, internal — not a Hydra blocker**), with Hydra review solicited non-blocking. T1–T3 (B) and C-a…C-f (C) green. See [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks). |
> | **F-SERVE-DIAG** | ⏳ **Layout + back-compat proven**, freeze on Nix-side criteria + soak | **maintainer sign-off** + an **in-tree consumer** (`nix log` over serve + the `ssh://` hook's fail-loud render) + the **≥1-cycle soak**; golden back-compat (D3.3) proven in-prototype and **ported into `src/libstore-tests`** (`serve-diag-core.cc`, `SERVE_PROTOCOL_VERSION` unbumped). **Hydra sign-off + queue-runner branch are solicited during the soak, not blockers** (revised 2026-06 — the frozen core is audited non-Hydra-specific). **Compatibility correction:** ship as serve **2.9** (minor bump within major 2), *not* `{3,0}` — a major bump is rejected by deployed clients at handshake before `min()` (decisions Blocker 3). |
>
> The honest one-liner: **F-INT resolves now; F-SERVE-DIAG freezes on Nix-side
> review + an in-tree consumer + a soak (Hydra invited, not blocking); F-WIRE's
> serve half (the deferred set, H3) waits on the Phase 3 design settling —
> internal, not a Hydra wait.** So there is **no hard external-Hydra freeze
> blocker** left: the D4 thread is a *review-solicitation*, not a gate.

## 0. The three freezes this plan gates (and why they are distinct)

Conflating these is the main reason Tier 2 reads as vague. They gate on
different evidence and can happen at different times:

| Freeze | What it locks | Public wire? | Gated by |
|---|---|---|---|
| **F-INT** — Phase 3 *internal* coordinator interface (spike §3) | the child↔coordinator control protocol, registry, replay/refcount semantics | **No** (machine-internal) | Workstream **A** |
| **F-WIRE** — Phase 3 *public* Build Session surface | the worker/serve additions for attach / `deduplicated` / `QueryActiveBuilds` | **Yes** (additive, version-gated) | Workstreams **B** + **C** proven (the worker-protocol ops are Nix-internal); its *serve-side* fields (**H3**) freeze later, gated on the Phase 3 design settling (design-maturity/internal), Hydra review solicited non-blocking — see [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks) |
| **F-SERVE-DIAG** — serve diagnostic core (Blocker 3) | `logRef`, `failurePhase`/`exitCode`/`logTail`, `QueryBuildLog`; bump `SERVE_PROTOCOL_VERSION` to `(2<<8\|9)` | **Yes** | Workstream **D** (the four Blocker-3 criteria) |

Key independence: **F-SERVE-DIAG does not need the coordinator.** The serve
diagnostic core (Phases 0–1) is orthogonal to dedup/attach, so Workstream D can
run fully in parallel with A/B/C. The operational fixes (Gaps A/B/C, Phases
0–2/4) gate on **nothing here** and ship first.

---

## The external gates (Hydra) — what is actually owed, by whom, and which freeze each blocks

Every other gate in this plan is engineering this repo can discharge alone —
and has: the A/B/C/D prototypes are green and the back-compat goldens are ported
into `src/libstore-tests`. This section names the *Hydra*-facing gates precisely,
because the two places the plan said "Hydra field set" (the **F-WIRE** row and
the **F-SERVE-DIAG** criteria) read like two separate hard asks when they are
not — and, after the **2026-06 revision**, only one of them is a freeze blocker
at all.

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
- **Only the *serve* surface ever touches Hydra**, and there the new fields split
  exactly as [Blocker 3](./remote-build-protocol-redesign.decisions.md#blocker-3--the-hydra-field-set--serve-diagnostic-core-freeze-rfc-q4-7-spike-51)
  splits them: the **frozen diagnostic core** (`logRef`,
  `failurePhase`/`exitCode`/`logTail`, `QueryBuildLog`) versus the **deferred
  set** (`builderId`, serve `deduplicated`, the §4.4 classification fields) that
  stays behind the *unstable* serve version until Phase 3 semantics settle.

**The audit that did the decoupling (revised 2026-06).** The diagnostic core was
already scoped to the generic set; a field-by-field check confirms **none of it
is Hydra-specific** — every field is justified by a *non-Hydra* serve consumer
(the `ssh://` `build-remote` hook and `nix log`/`nix build --store ssh://`):

| Frozen field / op | Justified without Hydra? | By what non-Hydra consumer |
|---|---|---|
| `logRef` | yes | `nix log` / any serve client fetching a persisted log needs the persist key (`LogStore::getBuildLog`) |
| `QueryBuildLog` (`Command = 10`) | yes | this *is* `nix log` over the serve path (Gap A/§4.5) — the `nix` CLI, not just Hydra |
| `failurePhase` | yes | fail-loud (G8) for any remote failure; same data local builds already render |
| `exitCode` | yes | every client rendering a remote failure wants the builder's exit status |
| `logTail` | yes | explicitly kept for the **non-Hydra** `ssh://` hook (fail-loud, G8) |

So the serve diagnostic core **freezes on Nix-side criteria** — maintainer
sign-off + an **in-tree consumer** (`nix log` over serve + the `ssh://` hook's
fail-loud render) + golden back-compat tests + the soak. Compatibility is
unconditionally handled by the `min()` handshake regardless of Hydra. What is
left is **no hard external-Hydra blocker** — one *deferred* gate (design-maturity,
internal) plus solicited input:

| # | Hydra-facing item | Blocks a freeze? | What it is | Owner |
|---|---|---|---|---|
| **H1** | Named-maintainer review of the diagnostic-core layout | **No — solicited, not blocking** (was D3.1) | request review via the coordination thread *during the soak*; if Hydra flags a problem while still unstable, revise before freezing (cheap). Absent/slow response does not hold the bump | Hydra queue-runner maintainer (reviewer) |
| **H2** | `hydra-queue-runner` consumer branch | **No — post-freeze adoption** (was D3.2) | Hydra adopts `QueryBuildLog` + the structured `BuildResult` when it chooses (G7 "never forced"); the additive/`min()` design means old Hydra keeps working meanwhile | Hydra queue-runner maintainer |
| **H3** | Deferred serve-set freeze | **Deferred, not externally Hydra-blocked**; blocks **F-WIRE** (serve half only) | `deduplicated`/`builderId` can't be frozen until the Phase 3 coordinator/dedup + elastic-backend design that *defines* their semantics settles — a **design-maturity** gate, which is *internal* Nix work, not a Hydra dependency. Hydra review of the eventual layout is solicited on the same non-blocking basis as H1 | libstore/serve-protocol (gated on Phase 3); Hydra = solicited reviewer |

Plus **D3.4** — the ≥1-release-cycle **soak** of the diagnostic-core layout on the
unstable serve version with no change. It is a clock, not an action; the ported
goldens (`serve-diag-core.cc`, with its `static_assert` pinning
`SERVE_PROTOCOL_VERSION == (2<<8|8)`) guard the layout *during* the soak, and
Hydra review (H1) is solicited in that same window.

**The relationships this makes explicit:**

- **F-SERVE-DIAG no longer has an external blocker.** It freezes on Nix-side
  sign-off + an in-tree consumer + golden back-compat (D3.3, already in-repo) +
  the soak. Hydra review rides the soak as input, not as a gate — so the build
  farm at `$WORK` can run the unstable core today and the upstream freeze does
  not wait on an external maintainer.
- **H3 is *deferred*, not externally Hydra-gated — the distinction matters.**
  The gate on `deduplicated`/`builderId` is that their *semantics aren't defined
  yet* (the Phase 3 coordinator/dedup + elastic-backend design is unfrozen);
  Blocker 3 forbids freezing a byte layout for semantics still in spike. That is
  **internal** design work, not a wait on Hydra. By the same audit applied to the
  core, these fields are if anything **elastic-backend / introspection-shaped,
  not Hydra-shaped**: Hydra picks its own builder (so `builderId` is largely
  redundant for it) and runs its own queue (so `deduplicated` is at most
  informational); they matter more to a client that *didn't* choose the builder
  (orchestrator backends, `QueryActiveBuilds`). When their freeze comes up the
  gate is "Phase 3 settled," with Hydra review solicited on the same non-blocking
  basis as the core. F-WIRE's worker-protocol ops remain Nix-internal (B+C).
- **Everything is version-gated both directions** (`min(client,server)` over the
  major-2 floor of Blocker 3's compatibility correction), so none of this is a
  flag day: a Hydra that has neither reviewed nor adopted anything keeps working
  byte-for-byte at ≤2.8 throughout.

**Sequencing.** Land the diagnostic core behind the unstable version (D1/D2,
done) → run the in-tree consumer + the soak, opening the coordination thread
(**D4**) in that window to gather H1 review → bump serve to `(2<<8|9)` on the
Nix-side criteria. **H2** (Hydra adoption) and **H3** (the deferred set, once
Phase 3 settles) follow afterwards. Until the Nix-side criteria + soak are met,
`SERVE_PROTOCOL_VERSION` stays at `(2<<8|8)`.

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

## Workstream D — serve diagnostic core (+ solicited Hydra review)  → gates **F-SERVE-DIAG**  [Blocker 3]

Independent of A/B/C. D1/D2 can land now behind the **unstable** version; the
bump to `SERVE_PROTOCOL_VERSION` gates on the four Nix-side criteria below.
**Revised 2026-06:** the Hydra sign-off and queue-runner branch are downgraded
from blockers to solicited input (the frozen core is audited non-Hydra-specific —
see [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks)).

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

**The four freeze criteria** (Blocker 3, revised 2026-06 — *all* must hold to bump to 2.9):

- [ ] **D3.1** — the **libstore/serve-protocol maintainer** has signed off on the
  exact field set and byte order. *(Hydra review is solicited during the soak as
  input, not as a blocker — see below.)*
- [ ] **D3.2** — at least one **in-tree serve consumer** exercises the core
  end-to-end: `nix log` over the serve path (`QueryBuildLog`) **and** the
  `ssh://` `build-remote` hook rendering `logTail`/`failurePhase`/`exitCode` on
  failure (fail-loud, §4.9). Proves the layout against a real consumer with no
  external dependency.
- [ ] **D3.3** — D2's golden tests prove round-trip at 2.8 and 2.9 **and** that a
  2.8 peer ignores 2.9 fields (full back-compat matrix, both directions).
- [ ] **D3.4** — the field set has soaked on the **unstable version for ≥1
  release cycle** with no layout change.

- **D4 — open the Hydra coordination thread** (RFC Q4 puts the coordination
  itself out of scope of the RFC) **during the soak, to solicit review** — no
  longer a freeze blocker. A ready-to-post **opening post is drafted** at
  [`*.hydra-coordination.md`](./remote-build-protocol-redesign.hydra-coordination.md)
  (the layout we are freezing, the back-compat matrix, and the questions for
  Hydra). If Hydra flags a problem while the version is still unstable, revise
  before freezing. The thread's later round (**H3**, the deferred serve set
  `deduplicated`/`builderId` for F-WIRE) is gated on the Phase 3 design settling —
  *internal design-maturity*, again with Hydra review solicited, not a standing
  external blocker.

**Owner:** libstore/serve-protocol maintainer (sign-off + serializer + version
bump); Hydra queue-runner maintainer is a solicited reviewer.

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
> bumping the wire. **After the 2026-06 revision, the remaining criteria are all
> closeable in-repo:** D3.1 (Nix-side maintainer sign-off), D3.2 (an in-tree
> consumer — `nix log` over serve + the `ssh://` hook), and D3.4 (the soak clock).
> Hydra review (D4) is solicited during the soak but no longer gates the bump.
> `SERVE_PROTOCOL_VERSION` stays unbumped until they hold.

---

## Sequencing (what unblocks what)

```
Phases 0–2/4 (Gaps A/B/C) ────────────────────────────────►  ship now, gate on nothing here

Workstream A ──► F-INT (freeze internal coordinator iface)
      └─► Workstream B (T1–T3) ─┐
      └─► Workstream C (C1+matrix) ─┴─► (+ serve-side fields, deferred to Phase 3) ──► F-WIRE

Workstream D:  D1+D2 (now, behind unstable) ──► D3.1(Nix sign-off)+D3.2(in-tree consumer)+D3.4(soak) ──► F-SERVE-DIAG
               (D4 Hydra review solicited during the soak, non-blocking)        (D is parallel to A/B/C)
```

- **A is the trunk** for dedup/attach: B and C extend its prototype.
- **B and C** are the F-WIRE preconditions on the Nix side; its serve-side fields
  (**H3**) freeze later, gated on the Phase 3 design settling — *internal*
  design-maturity, not an external Hydra wait — see [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).
- **D is parallel and independent**, and after the 2026-06 revision it has **no
  external blocker** — it freezes on Nix-side criteria + a soak; the D4 Hydra
  thread runs *during* the soak to gather review, not to gate the bump.

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
  validation pass. The reviewable package for that decision — the exact frozen
  surface, the invariants mapped to in-tree code, the per-gate evidence, the
  explicit non-asks, and the residual risks — is the
  [F-INT sign-off dossier](./remote-build-protocol-redesign.f-int-signoff.md).
  Nothing further is owed from a coding session.

**Freeze F-WIRE (Phase 3 public Build Session surface):** ⏳ Nix-side cleared;
the serve-side fields (**H3**) are deferred until the Phase 3 design settles —
*internal design-maturity, not an external Hydra blocker*.
- [x] F-INT done.
- [x] **T1, T2, T3** green (Workstream B prototype, `make check-b`).
- [x] **C1 + the `build-dedup-cancel` matrix (C-a…C-f)** green (Workstream C
  prototype, `make check-c`).
- [ ] **H3** — the *serve-side* fields of the Build Session surface (the deferred
  set: serve `deduplicated`/`builderId`). **Deferred, not externally Hydra-gated:**
  Blocker 3 forbids freezing them until the Phase 3 coordinator/dedup design that
  defines their semantics settles — a *design-maturity* gate (internal Nix work).
  By the core's audit these fields are elastic-backend/introspection-shaped, not
  Hydra-specific; Hydra review of the eventual layout is solicited, non-blocking.
  The worker-protocol ops carry no Hydra gate. See [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).

**Bump `SERVE_PROTOCOL_VERSION` → `(2<<8|9)` (F-SERVE-DIAG):** ⏳ layout + back-compat
proven; freezes on Nix-side criteria + soak (Hydra review solicited, not blocking — revised 2026-06).
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
- [ ] **D3.1** (libstore/serve-protocol maintainer sign-off on the field set +
  byte order) — Nix-side, in-repo. The reviewable package for this decision —
  the exact frozen field set + byte layout, the back-compat guarantees mapped to
  the golden cases, the in-tree consumers, and the one open layout item (the
  deferred set must be relocated off the `2.9` gate at freeze) — is the
  [F-SERVE-DIAG sign-off dossier](./remote-build-protocol-redesign.f-serve-diag-signoff.md).
- [ ] **D3.2** (an **in-tree** serve consumer exercises the core: `nix log` over
  serve + the `ssh://` hook's fail-loud render) — Nix-side, in-repo.
- [ ] **D3.4** (≥1-cycle soak on the unstable version) — time-gated; the clock
  starts once D3.1 fixes the layout. Hydra review (D4) is solicited in this
  window but does not gate the bump.
- *Solicited, not blocking (revised 2026-06):* a named Hydra maintainer's review
  and a `hydra-queue-runner` consumer branch — the frozen core is audited
  non-Hydra-specific. No hard external-Hydra freeze blocker remains; the deferred
  serve set (**H3**, for F-WIRE) is gated on the Phase 3 design settling
  (internal), not on Hydra. See [The external gates (Hydra)](#the-external-gates-hydra--what-is-actually-owed-by-whom-and-which-freeze-each-blocks).

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
| D — serve diagnostic core | F-SERVE-DIAG | libstore/serve-protocol (sign-off + bump); Hydra queue-runner maint. = solicited reviewer (non-blocking) |
