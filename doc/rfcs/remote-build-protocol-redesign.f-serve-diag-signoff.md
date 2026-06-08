# F-SERVE-DIAG sign-off dossier — the serve diagnostic-core field set

| | |
|------------------|------------------------------------------------|
| **Status**       | ⏳ **`D3.1` signed off (2026-06-08); `D3.2` satisfied; `D3.4` soak in progress (clock started 2026-06-08).** The layout is approved and fixed for the soak; the wire bump (and the deferred-set relocation) lands at soak completion — **not yet done**. `SERVE_PROTOCOL_VERSION` deliberately stays `2.8`. |
| **Gate**         | F-SERVE-DIAG (freeze the serve diagnostic core; bump `SERVE_PROTOCOL_VERSION` → `2.9` *at soak end*) |
| **Owner of the decision** | libstore/serve-protocol maintainer (`D3.1`). Hydra review (`D4`) is solicited but **non-blocking** |
| **Companion to** | the [validation plan](./remote-build-protocol-redesign.validation.md) (F-SERVE-DIAG checklist), the [decisions record](./remote-build-protocol-redesign.decisions.md) (Blocker 3), and [§4.4](./remote-build-protocol-redesign.md)/[§7](./remote-build-protocol-redesign.md) |

## 0. Decision record

**`D3.1` is signed off as of 2026-06-08, and the `D3.4` soak clock starts now.**
The diagnostic-core field set and its byte layout (§2, §3) are approved and
**fixed for the duration of the soak** on the strength of the proven back-compat
matrix (§3) and the in-tree consumers (§4).

- **Who:** the RFC owner, acting as the libstore/serve-protocol maintainer for
  this branch's RFC process — **not** an upstream NixOS organisation sign-off.
- **What is approved (`D3.1`):** the four diagnostic-core fields, their order,
  the `QueryBuildLog` op, and the freeze-time plan to relocate the deferred set
  off the `2.9` gate (§6). The layout is now treated as fixed; any change during
  the soak restarts the clock.
- **`D3.2` (satisfied):** in-tree consumers exercise the core — `nix log` over
  serve (`serve-build-log.sh`) and over `ssh-ng` (`ssh-ng-build-log.sh`), and the
  `ssh://` hook's fail-loud render consuming `logTail` (`build-remote-fail-loud.sh`).
- **`D3.4` (in progress, time-gated):** the ≥1-release-cycle soak on the
  *unstable* `2.9` version begins 2026-06-08. This is a calendar gate, not a
  decision — it cannot be shortened.
- **What is deliberately NOT done:** the wire is **not** bumped.
  `SERVE_PROTOCOL_VERSION` stays `2.8`; `2.9` is still offered only under the
  `serve-build-logs` experimental feature. The bump + deferred-set relocation +
  golden retarget is the freeze PR, which lands **after** the soak — this is the
  whole point of the soak (catch a layout bug while the bytes are still
  changeable, per §7's "cannot be walked back").

This document records and packages that decision. The original review request
follows unchanged.

## 1. The decision (`D3.1`: requested → granted)

> **Approve the serve diagnostic-core field set and its byte layout as the frozen
> `2.9` extension, so the one-release-cycle soak (`D3.4`) can begin.**

Ticked at sign-off (2026-06-08):

- [x] The **field set** frozen at `2.9` is exactly the **diagnostic core**:
  `logRef`, `failurePhase`, `exitCode`, `logTail` — and nothing else.
- [x] The **byte order** is acceptable: the four fields are appended **after**
  `builtOutputs`, in that order, `exitCode` as a `uint64`, each string using the
  existing empty-string-means-absent idiom — guarded `version >= {2,9}`.
- [x] The **`QueryBuildLog` serve command** (`Command = 10`, guarded `>= {2,9}`)
  is part of the freeze.
- [x] The **deferred set must be relocated** off the `2.9` gate at freeze — see
  §6, the one open layout item (this is the freeze-PR action, accepted as the plan).

Approving `D3.1` does **not** itself bump the wire; it fixes the layout so the
soak clock (`D3.4`) starts. The bump lands at the end of the soak.

## 2. What is being frozen

**The diagnostic core** — four fields on `BuildResult`
(`src/libstore/include/nix/store/build-result.hh`), serialized by the serve
`BuildResult` serializer (`src/libstore/serve-protocol.cc`) when
`conn.version >= {2,9}`:

| Field | Type on the wire | Meaning |
|---|---|---|
| `logRef` | string | the resolved-drv key under which the builder persisted the log, so a client can `nix log --store <builder> <logRef>` |
| `failurePhase` | string | for a failure, the phase that failed (e.g. `"build"`), if known |
| `exitCode` | `uint64` (held as `int64_t`) | for a failure, the builder's exit status (0 if N/A) |
| `logTail` | string | for a failure, the tail of the build log, so a remote failure renders inline with no second round-trip |

**The `QueryBuildLog` op** — serve `Command = 10`
(`src/libstore/include/nix/store/serve-protocol.hh`), and its worker-protocol
parallel `QueryBuildLog = 48` gated on the `build-log-query` feature
(`worker-protocol.hh`) — i.e. `nix log` over `ssh-ng`/`ssh://`.

**The version mechanics** (`serve-protocol.hh`, `serve-protocol-connection.cc`):
`SERVE_PROTOCOL_VERSION` (`latest`) stays `(2<<8|8)` = `2.8`;
`unstableDiagnostics = {2,9}` is offered **only** when the `serve-build-logs`
experimental feature is on (`offeredVersion()`); both peers negotiate
`min(remote, local)`, so a peer that does not offer `2.9` transparently degrades
to `2.8`. **The freeze is the act of bumping `latest` to `2.9`** (and dropping
the experimental gate on offering it).

## 3. Back-compat guarantees — and the golden evidence for each

The byte-level characterisation lives in
`src/libstore-tests/serve-diag-core.cc` (9 gtest cases). It models the
**candidate** `2.9` layout *without* touching the production serializer, and a
`static_assert(SERVE_PROTOCOL_VERSION == (2<<8|8))` pins it pre-freeze, so the
goldens catch any accidental drift while the layout soaks. At freeze the goldens
retarget the real serializer and the self-contained model is deleted.

| Guarantee | Golden case |
|---|---|
| **Additive layout** — `2.9` bytes are exactly `2.8` bytes **plus** the appended diagnostic-core tail | `additiveLayout` (`b29.size() > b28.size()`, `b29` starts with `b28`) |
| **Old reader, new bytes** — a `2.8` reader consumes exactly the `2.8` prefix and ignores the tail (no desync) | `readsExactly_2_8_from_2_9_bytes` |
| **Round-trip** at the negotiated version | `roundTripBuildResult` |
| **Byte stability** — the layout is frozen against a fixed sample | `goldenBytesStable` |
| **Negotiated-down** — a `2.8`-negotiated peer emits no tail | (matrix cases) |
| **Major-bump rejection guard** — a `{3,0}` peer is rejected by the client handshake (which runs *before* `min()`), which is exactly why the core ships as a **minor** `{2,9}`, not `{3,0}` | the `{3,0}`-rejection regression case |

The handshake is `min()`-gated in both directions and additive: old Hydra ↔ new
Nix and new Hydra ↔ old Nix both work, with no coordinated flag day (the §4.8 /
§7 constraint).

## 4. In-tree consumers (`D3.2`)

The core is exercised by real in-tree consumers, not only the golden model:

| Consumer test | Exercises |
|---|---|
| `tests/functional/serve-build-log.sh` | `nix log` over the serve (`ssh://`) path — `QueryBuildLog` end to end |
| `tests/functional/ssh-ng-build-log.sh` | `nix log` over `ssh-ng` (worker `QueryBuildLog` / the `getBuildLogExact` gap) |
| `tests/functional/build-remote-fail-loud.sh` | the `ssh://` build hook's fail-loud render, which consumes `logTail` |
| `tests/functional/build-remote-serve-log-stream.sh` | live log streaming over the serve path |

These also demonstrate the core is **not Hydra-specific**: the `nix` CLI and the
`ssh://` build hook are the consumers driving it, which is the basis for the
2026-06 revision making Hydra review solicited rather than blocking.

## 5. What this sign-off does **not** cover

1. **The deferred dedup/fleet set (H3)** — `deduplicated`, `builderId`, and the
   §4.4 failure-classification pair `failureClass` / `resourceHint`. The fields
   exist in `BuildResult` and serialize today on the unstable `2.9` wire (after
   the diagnostic core), but their *semantics* are defined by the Build Registry
   / elastic-backend design and depend on the Phase 3 design settling
   (Blocker 3), so their wire layout must **not** freeze at `2.9`. See §6 — this
   is the one thing the freeze must actively keep out.
2. **The internal coordinator interface** — that is F-INT, with its own
   [dossier](./remote-build-protocol-redesign.f-int-signoff.md).
3. **`D3.4`, the soak.** `D3.1` fixes the layout; the ≥1-release-cycle soak on
   the unstable `2.9` version then runs (clock starts at sign-off) before the
   actual `latest` bump. Time-gated, not a coding item.

## 6. The one open layout item the review must settle

**The deferred set currently shares the `2.9` gate in the production
serializer.** `serve-protocol.cc` writes/reads the deferred set —
`builderId`, `deduplicated`, and the §4.4 `failureClass` / `resourceHint` pair —
under the *same* `version >= {2,9}` guard as the frozen core, and the
`build-result.hh` comment documents this as deliberate "under the same unstable
gate" behaviour — which is harmless **today** (the `2.9` wire is unstable and
only offered under an experimental feature, so both peers agree).

But the **candidate frozen layout** in `serve-diag-core.cc` deliberately splits
them: the diagnostic core at `>= {2,9}`, and the whole deferred set at a *later*
unstable version (`{2,99}` in the model). Freezing `2.9` therefore is **not** a
pure "retarget the goldens" step: the freeze PR must **move the deferred set
(`builderId`, `deduplicated`, `failureClass`, `resourceHint`) to a later unstable
gate** so the bump to `latest = 2.9` freezes the core *only* and leaves the
deferred set unfrozen (Blocker 3 / §7 guardrail 7: new fields go after the
version guard and the deferred set stays behind an unstable version until its
semantics settle).

**Decision for `D3.1`:** approve the diagnostic core at `2.9` **and** the
relocation of the deferred set to a later unstable gate as part of the freeze.
(No code is changed by this dossier; the relocation is the freeze PR's job, after
sign-off, so it doesn't pre-empt the maintainer's layout call.)

## 7. Why the stakes here are higher than F-INT — and what bounds them

Unlike F-INT, this freeze **is** on the client wire, and §7 says a frozen wire
layout cannot be walked back. That is exactly why the bar is higher: a proven
back-compat matrix (§3) **and** a soak (`D3.4`), not just engineering evidence.
What bounds the risk is the design's additivity — the core is appended after a
version guard, so `2.8` peers are byte-for-byte unaffected — and the discipline
of §6: only the genuinely generic, consumer-proven fields freeze; everything
whose semantics are still in motion stays behind a later unstable version. The
sign-off being requested is precisely "these four fields and this order are the
ones we are willing to never change," with the soak as the safety margin.
