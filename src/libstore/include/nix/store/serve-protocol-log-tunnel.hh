#pragma once
///@file

#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"

namespace nix {

/**
 * Serve-side stderr log tunnel: frames a build's log activity as `STDERR_*`
 * messages onto the serve connection, mirroring the worker protocol's
 * `TunnelLogger` (daemon.cc). Only used behind the unstable serve 2.9 /
 * `serve-build-logs` gate, so a 2.8 peer never sees it. Build errors still use
 * the existing result encoding: the frame stream ends with `STDERR_LAST`,
 * never `STDERR_ERROR`.
 *
 * Lives in libstore (not file-local in `nix-store --serve`) so the serve wire
 * path is unit-testable.
 */
struct ServeTunnelLogger : Logger
{
    BufferedSink & to;
    bool canSend = false;
    std::vector<std::string> pending;

    ServeTunnelLogger(BufferedSink & to)
        : to(to)
    {
    }

    void enqueue(std::string s);

    void log(Verbosity lvl, std::string_view s) override;

    void logEI(const ErrorInfo & ei) override;

    void startActivity(
        ActivityId act,
        Verbosity lvl,
        ActivityType type,
        const std::string & s,
        const Fields & fields,
        ActivityId parent) override;

    void stopActivity(ActivityId act) override;

    void result(ActivityId act, ResultType type, const Fields & fields) override;

    /** Begin streaming: flush anything buffered before the build started. */
    void startWork();

    /** End the log-frame stream; the result bytes follow. */
    void stopWork();
};

} // namespace nix
