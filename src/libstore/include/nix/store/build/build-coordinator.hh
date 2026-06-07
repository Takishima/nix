#pragma once
///@file
///
/// Stock-daemon **build coordinator** (RFC `remote-build-protocol-redesign`
/// Phase 3 / spike §3; decision recorded in
/// `doc/rfcs/prototypes/phase-3-coordination-decision.md`). The fork-per-
/// connection `nix-daemon` shares no `Worker` across connections, so cross-
/// client dedup/attach/replay (G3) needs a component that outlives individual
/// connections: a long-lived per-store **coordinator process** that owns the
/// `BuildRegistry`, runs the real builds, and fans their log out to every
/// attached daemon child.
///
/// **Expand/contract.** This lands as a *parallel change*: the relay is an
/// additive, env-gated branch in the daemon's `BuildDerivation` handler
/// (`NIX_BUILD_COORDINATOR_SOCKET`); the existing in-process build path is the
/// untouched default. The planned *contraction* — once the coordinator is the
/// proven default for daemons fronting one — is to promote it behind a real
/// experimental feature, collapse the gate, and retire the duplicated
/// direct-build branch. Until then nothing existing is removed.
///
/// Everything here is **below the client wire** (the daemon child still speaks
/// ordinary worker-protocol `STDERR_*` to its client via the existing
/// `TunnelLogger`), so there is no flag day (spike §5.1).

#include <string>

#include "nix/store/build-result.hh"
#include "nix/store/derivations.hh"
#include "nix/store/path.hh"
#include "nix/store/store-api.hh"

namespace nix {

class Logger;

/**
 * Relay a `BuildDerivation` to the coordinator at `socketPath` instead of
 * building in-process (the daemon-child side of the mechanism). Connects
 * (lazily spawning the coordinator if absent), issues `START_OR_ATTACH` keyed
 * on the *resolved* derivation `drvPath` (a `BasicDerivation` already carries
 * concrete inputs — guardrail §8.1 #1), re-emits the coordinator's log frames
 * to `logger` (which, inside a daemon op, is the `TunnelLogger` → the client
 * sees no difference), and returns the shared build's `BuildResult` with
 * `deduplicated` set. Client disconnect / interrupt closes the control socket,
 * which the coordinator treats as a refcounted unsubscribe (Blocker 2,
 * guardrail §8.1 #3) — it does **not** unconditionally cancel a build other
 * clients still want.
 *
 * The store the coordinator opens to run the build (the same physical store the
 * daemon serves) is derived from `store` itself, so it round-trips even for a
 * `--store /path` builder. The coordinator socket is `NIX_BUILD_COORDINATOR_SOCKET`
 * if set, else `$stateDir/coordinator.socket` (O1).
 */
BuildResult relayBuildToCoordinator(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted);

/**
 * The coordinator main loop: bind `socketPath`, host the in-memory
 * `BuildRegistry`, and serve daemon-child control connections — single-threaded
 * event loop (O4), builds run in forked children (isolation preserved, spike
 * §2.2), replay buffer in coordinator memory (O5). Returns when the coordinator
 * idle-exits. `storeUri` is the store builds run against.
 */
[[noreturn]] void runBuildCoordinator(const std::string & socketPath, const std::string & storeUri);

} // namespace nix
