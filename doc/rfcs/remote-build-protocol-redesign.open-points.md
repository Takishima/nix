# Open points: what remains to be clarified across the redesign document set

| | |
|------------------|------------------------------------------------|
| **Status**       | Review of open items (reading pass, not a decision) |
| **Covers**       | [`*.md`](./remote-build-protocol-redesign.md), [`*.decisions.md`](./remote-build-protocol-redesign.decisions.md), [`*.spike.md`](./remote-build-protocol-redesign.spike.md), and the two `*.review.md` files |
| **Purpose**      | Consolidate every point that is still genuinely unsettled, so the next contributor knows exactly what is decided vs. owed |

> This document does **not** introduce new design. It reads the five existing
> documents end to end and collects the points that are still open, the points
> that are decided *in principle* but not yet validated (and therefore gate a
> freeze), and a few internal-consistency nits. Each item cites the document and
> section it comes from so it can be actioned in place.

## 0. What is already settled (so the open list is read in context)

The set is mature and internally consistent. For the avoidance of doubt, these
are **closed** and should not be reopened by this document:

* **The operational core — Gaps A/B/C (RFC §2.6–2.7, Phases 0–2/4).** `nix log`
  over `ssh-ng` (Gap A), the serve-hop log drop (Gap B), and fail-loud / logs
  hidden by default (Gap C) are fully specified and need no shared-state work.
* **RFC Q0** (cross-process mechanism = coordinator process) and **Q1's
  *location*** (replay buffer in coordinator memory) — resolved by the spike
  (spike §2.5, §3.5).
* **RFC Q2** (refcounted-cancel matrix), **Q3** (CA-resolution timing), **Q4**
  (Hydra field set / serve-3.0 split), **Q7** (reuse-key vs. trust) — resolved
  in the decisions record (Blockers 1–3).

Everything below is what is *not* in that list.

## 1. Genuinely open — needs a human decision

These have no decision recorded in any of the five documents; they are carried
explicitly as open questions.

| # | Open point | Source | Why it matters / what is needed |
|---|---|---|---|
| 1.1 | **Coordinator deployment model** — a new `nix-daemon` sub-role vs. a sidecar binary: who starts it, who reaps it, where the socket lives, behaviour under `systemd` / multi-user daemon setups. | spike §5.4.3, §6 Q1 (RFC §4.3.3.1) | Everything downstream of the coordinator (lifecycle, socket security in §3.7.1, lazy-spawn in 1.4) inherits this. The spike prototypes a sidecar but explicitly does not settle production ownership. |
| 1.2 | **Coordinator crash-recovery posture** — is "degrade safely to today's `PathLocks` behaviour, losing in-flight fan-out" acceptable, or must builds survive a coordinator restart (persistent registry + re-adoption of running build subprocesses)? | spike §6 Q5 (§2.2 cleanup row) | A real robustness/cost decision. The spike argues the safe-degrade floor by inspection only; full supervision/restart is named as productionization, not designed. |
| 1.3 | **Coordinator throughput / concurrency ceiling** — single event loop vs. sharded/multi-threaded design. Every build and every log frame for every connection funnels through one coordinator (O(frames × subscribers) centrally). | spike §6 Q7; spike-review §2.3 | In direct tension with **G6** (elastic backends), the very scenario the coordinator partly exists to serve. Only a *measurement* is planned (spike §4.2); no decision on the ceiling. |
| 1.4 | **Lazy-spawn lifecycle posture** — the spawn-election (two children racing to spawn) and the "decline-and-respawn" handshake (idle-exit between connect and first use). Election primitive, idle-exit grace, and spawn-at-daemon-start vs. lazily. | spike §3.1, §6 Q8; spike-review §2.4 | Sketched but not specified. Without it a flaky spawn is misread as a coordination bug. |
| 1.5 | **`QueryActiveBuilds` default privacy** for untrusted callers — aggregate count vs. nothing. | RFC §10 Q5; spike §6 Q6 | The coordinator enforces whatever is chosen (single chokepoint, spike §3.7.3), but the policy itself is a project call, unmade. |
| 1.6 | **Elastic-capacity advertisement** — how a builder declares "I self-schedule / have elastic capacity" (a new machines-spec / store-config field vs. handshake negotiation), and how `maxJobs` degrades to a hint **without** silently overcommitting existing `/etc/nix/machines` files. | RFC §10 Q6 (§4.7.1) | The RFC fixes the *constraint* (must be strictly opt-in; default hard-cap semantics unchanged) but not the *mechanism*. This is the bulk of **G6 / Phase 6**. |
| 1.7 | **Replay cap default value + truncation-marker UX** — the *location* is decided (coordinator memory); the concrete byte cap (proposed ~4 MiB) and the head+tail `…truncated N frames…` presentation for very long builds (kernel/LLVM) are a tuning/UX call. | RFC §10 Q1 (remainder); spike §3.5, §6 Q4 | The only part of Q1 still open. Low risk, but unset. |

## 2. Decided in principle, but not yet validated (these gate a freeze)

The decisions record converges the three blockers, but each carries a follow-up
that must land **before** the Phase 3 wire surface or serve 3.0 can be frozen.
These are not reopenings — they are the validation debt the decisions themselves
name.

* **2.1 — CA `resolving`→promote path is unproven (Blocker 1).** The key/auth
  *rule* is frozen, but the decisions record calls the CA `resolving`→promote +
  re-authorize-on-promotion check "the one remaining unproven mechanism"
  (decisions §1.5; spike §3.8, §4.3). The first prototype targets only the
  **input-addressed** path. **Owed:** the CA-merge prototype and trust tests
  (1)–(3) (existence-oracle, CA-merge re-auth, asserted-key spoof) **before any
  Phase 3 wire freeze**.

* **2.2 — Refcounted-cancel is stubbed in the prototype (Blocker 2).** The
  state→action table is agreed and changes no wire, but
  `hasRootReasonToContinue()` (explicit-root-only) and the
  per-subscriber-deadline / max-envelope timer are stubbed to `false` in the
  spike (spike §5.2). **Owed:** implement both in the coordinator and encode the
  table as the `build-dedup-cancel` functional test. The decisions record notes
  the table "must be agreed before Phase 3 implementation" (decisions §2.5).

* **2.3 — Serve 3.0 cannot freeze yet (Blocker 3).** The diagnostic core is
  chosen, but the four freeze criteria are **unmet**, and two are blocking:
  - the sign-off authority is literally **"Hydra queue-runner maintainer
    (TBD)"** (decisions §3.5, summary table) — *no human is named yet*;
  - the **Hydra coordination thread is not yet open** (RFC Q4 puts coordination
    out of scope of the RFC; the decisions record assigns the *role* and gate,
    not a person);
  - a working `hydra-queue-runner` branch and golden/characterisation tests plus
    a ≥1-cycle soak on the unstable version are all still ahead.
  Until these hold, `SERVE_PROTOCOL_VERSION` must stay at 2.8 and the fields live
  behind the unstable gate. **This is the single biggest external dependency in
  the whole set.**

## 3. Internal-consistency nits (cheap cleanups)

* **3.1 — RFC §4.3 carries pre-decision wording.** The §4.3 prose still lumps
  `--keep-going` with build roots and then self-corrects mid-section ("refining
  this section's earlier wording"). Now that Blocker 2 is decided, §4.3 should
  state the refcount + explicit-root-only rule directly rather than reading as a
  correction of itself.
* **3.2 — RFC §10 item 1 lacks a status tag.** Q2/Q3/Q4/Q7 are marked
  **✅ RESOLVED**; item 1 (replay policy) is not, even though the §10 preamble
  says the spike resolved its *location*. Tagging it "✅ RESOLVED (location;
  cap/UX = open, see 1.7)" would make the half-resolved state legible.
* **3.3 — Owner placeholders.** Blocker 3's owner (decisions §3.5 and the
  summary table) is an unfilled role ("TBD"). Naming it is a prerequisite for
  freeze criterion 1, so it is worth tracking as an explicit action, not just a
  table cell.

## 4. One-line summary

The operational half of the design (Gaps A/B/C) is decision-complete and ready.
The dedup/attach half is *architecturally* settled (coordinator process, frozen
key/auth rule, agreed cancel matrix, chosen serve-3.0 core) but still owes: a
handful of **deployment/operational decisions** (§1: deployment model,
crash-recovery, throughput ceiling, lazy-spawn, query privacy, elastic
advertisement, replay tuning), three **validation prototypes/tests** that gate
the Phase 3 freeze (§2), and — the long pole — a **named Hydra maintainer and an
open coordination thread** before serve 3.0 can be frozen (§2.3).
