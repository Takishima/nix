#pragma once
///@file
///
/// The per-store **build coordinator**: a long-lived process that owns
/// the `BuildRegistry`, runs the real builds, and fans their log out to
/// every attached client. It exists because the fork-per-connection
/// daemon shares no `Worker` across connections. Everything here is below
/// the client wire, so there is no flag day.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nix/util/error.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/store/build-result.hh"
#include "nix/store/build/build-registry.hh"
#include "nix/store/derivations.hh"
#include "nix/store/path.hh"
#include "nix/store/store-api.hh"

namespace nix {

class Logger;

/**
 * Coordination is impossible at this store's socket location — typically
 * an unprivileged process whose coordinator path lives in a directory it
 * cannot create files in (e.g. a non-root `nix-daemon --stdio` against
 * `/nix/var/nix`). Thrown before any build work has started, so callers
 * degrade to an ordinary uncoordinated build (with a warning) instead of
 * failing the build. Distinct from the transient lost-election race
 * (`std::nullopt` from `tryStartCoordinatorRelay`), which *is* retried.
 */
MakeError(CoordinatorUnavailable, Error);

/**
 * Relay a build to the coordinator instead of building in-process:
 * connect (lazily spawning the coordinator if absent), issue
 * `START_OR_ATTACH` keyed on the resolved derivation, re-emit the
 * coordinator's log frames to `logger`, and return the shared build's
 * `BuildResult`. Disconnect/interrupt closes the control socket, which
 * the coordinator treats as a refcounted unsubscribe — never an
 * unconditional cancel of a build other clients still want.
 */
BuildResult relayBuildToCoordinator(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted);

/**
 * The relay's incremental byte → record → logger pump: feed it whatever
 * bytes arrive (any framing split); it surfaces log frames as
 * `resBuildLogLine` results — like a non-relayed build — and returns the
 * decoded `BuildResult` once the result record completes.
 */
class CoordinatorRelayPump
{
public:
    CoordinatorRelayPump(Logger & logger, const std::string & drvPathStr);
    ~CoordinatorRelayPump();
    CoordinatorRelayPump(CoordinatorRelayPump &&) noexcept;
    CoordinatorRelayPump & operator=(CoordinatorRelayPump &&) noexcept;

    /**
     * Feed raw control-socket bytes. Returns the final `BuildResult` once the
     * result record has been fully received.
     */
    std::optional<BuildResult> feed(std::string_view data);

    /**
     * Whether any bytes have been fed. EOF before any byte is the
     * retryable idle-exit race; EOF mid-stream is a real failure.
     */
    bool receivedAnything() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/**
 * An in-flight relay subscription for event-loop callers: watch `socket`
 * for readability and `feed` the pump until it yields the `BuildResult`.
 * Dropping the session closes the socket (a refcounted unsubscribe).
 */
struct CoordinatorRelaySession
{
    AutoCloseFD socket;
    CoordinatorRelayPump pump;
};

/**
 * One *non-blocking* attempt to connect to `store`'s coordinator and
 * subscribe to this build. `std::nullopt` only in the narrow lost-election
 * race — the caller chooses how to wait before retrying. An unusable
 * socket location throws `CoordinatorUnavailable` instead of looking like
 * a coordinator that never comes up; callers degrade to an uncoordinated
 * build.
 */
std::optional<CoordinatorRelaySession> tryStartCoordinatorRelay(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted);

/**
 * The blocking form of `tryStartCoordinatorRelay`: retries the
 * lost-election race with a short backoff.
 */
CoordinatorRelaySession startCoordinatorRelay(
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
/// coordinator → child: the attach succeeded. Sent promptly so a relay can
/// bound its handshake: a registry MISS is otherwise legitimately silent
/// until the build's first log line, which is indistinguishable from a
/// wedged coordinator. Decoders ignore unknown tags, so this is
/// compatible in both directions.
constexpr char MSG_ATTACHED = 'K';
/// coordinator → child: the QUERY_ACTIVE answer (a JSON array of active builds).
constexpr char MSG_ACTIVE = 'A';
} // namespace coordinator_proto

/**
 * Record-level entry point over `CoordinatorRelayPump`, so what a relayed
 * build emits into the logger is unit-testable. `readRecord` yields one
 * record body per call, nullopt = EOF.
 */
BuildResult processCoordinatorRelayRecords(
    const std::function<std::optional<std::string>()> & readRecord, Logger & logger, const std::string & drvPathStr);

/**
 * Read-only introspection: the coordinator's authorization-filtered view
 * of in-flight builds. Never spawns a coordinator — no coordinator simply
 * means nothing is in flight.
 */
std::vector<ActiveBuildStatus> queryActiveBuildsViaCoordinator(Store & store);

} // namespace nix
