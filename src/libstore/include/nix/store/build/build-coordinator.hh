#pragma once
///@file
///
/// Stock-daemon **build coordinator**. The fork-per-connection `nix-daemon`
/// shares no `Worker` across connections, so cross-client dedup/attach/replay
/// needs a component that outlives individual connections: a long-lived
/// per-store **coordinator process** that owns the `BuildRegistry`, runs the
/// real builds, and fans their log out to every attached daemon child.
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
/// `TunnelLogger`), so there is no flag day.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nix/store/build-result.hh"
#include "nix/store/build/build-registry.hh"
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
 * concrete inputs), re-emits the coordinator's log frames to `logger` (which,
 * inside a daemon op, is the `TunnelLogger` → the client sees no difference),
 * and returns the shared build's `BuildResult` with `deduplicated` set. Client
 * disconnect / interrupt closes the control socket, which the coordinator
 * treats as a refcounted unsubscribe — it does **not** unconditionally cancel a
 * build other clients still want.
 *
 * The store the coordinator opens to run the build (the same physical store the
 * daemon serves) is derived from `store` itself, so it round-trips even for a
 * `--store /path` builder. The coordinator socket is `NIX_BUILD_COORDINATOR_SOCKET`
 * if set, else `$stateDir/coordinator.socket`.
 */
BuildResult relayBuildToCoordinator(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted);

/**
 * Coordinator control-protocol record tags (internal mechanism, never the
 * public client wire); in the header so the unit tests share them.
 */
namespace coordinator_proto {
/// child → coordinator
constexpr char MSG_START_OR_ATTACH = 'S';
/// child → coordinator: read-only introspection, answered with one MSG_ACTIVE.
constexpr char MSG_QUERY_ACTIVE = 'Q';
/// coordinator → child (or build-child → coordinator on the build pipe)
constexpr char MSG_FRAME = 'F';
constexpr char MSG_RESULT = 'R';
/// coordinator → child: the QUERY_ACTIVE answer (a JSON array of active builds).
constexpr char MSG_ACTIVE = 'A';
} // namespace coordinator_proto

/**
 * The relay's record → logger pump, factored out of `relayBuildToCoordinator`
 * so what a relayed build emits into the logger is unit-testable. Drains
 * records from `readRecord` (one body per call, nullopt = EOF), surfacing
 * `MSG_FRAME` log frames as `resBuildLogLine` results under an `actBuild`
 * activity — like a non-relayed build — until `MSG_RESULT`, whose decoded
 * `BuildResult` is returned.
 */
BuildResult processCoordinatorRelayRecords(
    const std::function<std::optional<std::string>()> & readRecord, Logger & logger, const std::string & drvPathStr);

/**
 * The coordinator main loop: bind `socketPath`, host the in-memory
 * `BuildRegistry`, and serve daemon-child control connections — single-threaded
 * event loop, builds run in forked children (isolation preserved), replay
 * buffer in coordinator memory. Returns when the coordinator idle-exits.
 * `storeUri` is the store builds run against.
 */
[[noreturn]] void runBuildCoordinator(const std::string & socketPath, const std::string & storeUri);

/**
 * Read-only introspection: ask the coordinator fronting `store` for its
 * in-flight builds. Connects to an *existing* coordinator at `store`'s socket —
 * it does **not** spawn one, so when no coordinator is running there are simply
 * no active builds and the result is empty. The snapshot is the coordinator's
 * authorization-filtered view (`BuildRegistry::queryActive`); on the local
 * peer-cred socket that is "builds this uid may observe".
 */
std::vector<ActiveBuildStatus> queryActiveBuildsViaCoordinator(Store & store);

} // namespace nix
