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

Most of this tier is now resolved by operational decisions O1–O5 (rows marked
✅). The two still genuinely open are **1.5** (`QueryActiveBuilds` privacy
default) and **1.6** (elastic-capacity advertisement) — both carry product/policy
judgment rather than a purely technical answer.

| # | Open point | Source | Why it matters / what is needed |
|---|---|---|---|
| 1.1 | ✅ **RESOLVED — Coordinator deployment model.** Daemon binary in a `--coordinator` role; a separate process supervised/reaped by the `daemonLoop` parent (not a separate codebase, not in-process with the listener); one per store at `$NIX_STATE_DIR/coordinator.socket` (`0660`, peer-cred-verified); run as root, one per machine, **not** per-user; lazy spawn as the no-daemon fallback. See decisions record [O1](./remote-build-protocol-redesign.decisions.md#operational-decision-o1--coordinator-deployment-model-spike-6-q1-open-points-11). | spike §6 Q1; RFC §4.3.3.1 | *(Daemon-restart survival deferred to 1.2.)* |
| 1.2 | ✅ **RESOLVED — Coordinator crash-recovery posture.** v1 is safe-degrade, no persistence/re-adoption: builders are forked `dieWithParent`/in a coordinator-killable cgroup so a crash releases their `PathLocks`; relay children fall back to building locally (coalescing via `PathLocks`); the existing lock/validity logic guarantees no corruption/double-build. Persistent registry + re-adoption deferred as evidence-gated hardening. See decisions record [O2](./remote-build-protocol-redesign.decisions.md#operational-decision-o2--coordinator-crash-recovery-posture-spike-6-q5-open-points-12). | spike §6 Q5; §2.2 | — |
| 1.3 | ✅ **RESOLVED (posture) — Coordinator throughput.** v1 = single-threaded event loop; per-build-key sharding deferred and gated on the spike §4.2 throughput measurement; below the wire, so a later sharded design is an internal change. See decisions record [O4](./remote-build-protocol-redesign.decisions.md#operational-decision-o4--coordinator-throughput-posture-spike-6-q7-open-points-13). | spike §6 Q7; spike-review §2.3 | — |
| 1.4 | ✅ **RESOLVED — Lazy-spawn lifecycle.** Daemon parent is sole spawner (lazy, at first capability-negotiated request); fallback uses a `flock`/`O_EXCL` + socket-`bind` election, stale-socket reclaim, configurable idle-exit grace (default 10 min), and a `GOING_AWAY`/decline-and-respawn handshake. See decisions record [O3](./remote-build-protocol-redesign.decisions.md#operational-decision-o3--lazy-spawn-lifecycle-spike-6-q8-open-points-14). | spike §3.1, §6 Q8; spike-review §2.4 | — |
| 1.5 | **`QueryActiveBuilds` default privacy** for untrusted callers — aggregate count vs. nothing. | RFC §10 Q5; spike §6 Q6 | The coordinator enforces whatever is chosen (single chokepoint, spike §3.7.3), but the policy itself is a project call, unmade. |
| 1.6 | **Elastic-capacity advertisement** — how a builder declares "I self-schedule / have elastic capacity" (a new machines-spec / store-config field vs. handshake negotiation), and how `maxJobs` degrades to a hint **without** silently overcommitting existing `/etc/nix/machines` files. | RFC §10 Q6 (§4.7.1) | The RFC fixes the *constraint* (must be strictly opt-in; default hard-cap semantics unchanged) but not the *mechanism*. This is the bulk of **G6 / Phase 6**. |
| 1.7 | ✅ **RESOLVED — Replay cap + truncation UX.** Byte cap, default 4 MiB (configurable); over the cap, ~1 MiB head + ~3 MiB tail with an explicit `…N frames / M bytes truncated…` marker frame; `replayed=true` tagging; post-build handoff to the persisted log via `QueryBuildLog`. See decisions record [O5](./remote-build-protocol-redesign.decisions.md#operational-decision-o5--replay-cap-default-and-truncation-ux-rfc-q1-remainder-spike-6-q4-open-points-17). | RFC §10 Q1 (remainder); spike §3.5, §6 Q4 | — |

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
handful of **deployment/operational decisions** (§1; deployment, crash-recovery,
throughput posture, lazy-spawn, and replay tuning are now decided — O1–O5 —
leaving only `QueryActiveBuilds` privacy and elastic-capacity advertisement),
three **validation prototypes/tests** that gate
the Phase 3 freeze (§2), and — the long pole — a **named Hydra maintainer and an
open coordination thread** before serve 3.0 can be frozen (§2.3).
