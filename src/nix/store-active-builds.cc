#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/store/build/build-coordinator.hh"
#include "nix/util/experimental-features.hh"

#include <nlohmann/json.hpp>

#include <ctime>

namespace nix {

static std::string renderBytes(uint64_t n)
{
    static const char * units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double) n;
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(units)) {
        v /= 1024.0;
        ++u;
    }
    return u == 0 ? fmt("%d B", n) : fmt("%.1f %s", v, units[u]);
}

static std::string renderAge(time_t startTime, time_t now)
{
    auto secs = now > startTime ? now - startTime : 0;
    if (secs < 60)
        return fmt("%ds", secs);
    if (secs < 3600)
        return fmt("%dm%02ds", secs / 60, secs % 60);
    return fmt("%dh%02dm", secs / 3600, (secs % 3600) / 60);
}

struct CmdActiveBuilds : StoreCommand, MixJSON
{
    std::string description() override
    {
        return "show the builds the build coordinator is currently running";
    }

    std::string doc() override
    {
        return
#include "store-active-builds.md"
            ;
    }

    /** Introspection is part of the coordinator's dedup/attach machinery. */
    std::optional<ExperimentalFeature> experimentalFeature() override
    {
        return Xp::BuildCoordinator;
    }

    void run(ref<Store> store) override
    {
        // The leaf of a `NixMultiCommand` is not auto-gated by
        // `experimentalFeature()` (only the top-level command is), so enforce it
        // here: introspection is meaningless without the coordinator anyway.
        experimentalFeatureSettings.require(Xp::BuildCoordinator);

        auto builds = queryActiveBuildsViaCoordinator(*store);
        auto now = time(nullptr);

        if (json) {
            auto arr = nlohmann::json::array();
            for (const auto & b : builds)
                arr.push_back({
                    {"resolvedDrv", b.key.resolvedDrv},
                    {"startTime", b.startTime},
                    {"subscribers", b.subscriberCount},
                    {"logBytes", b.logBytes},
                    {"rooted", b.rooted},
                });
            printJSON(arr);
            return;
        }

        if (builds.empty()) {
            notice("No builds are currently in flight.");
            return;
        }

        notice("%-7s  %-4s  %-9s  %-6s  %s", "age", "subs", "log", "rooted", "resolved derivation");
        for (const auto & b : builds)
            notice(
                "%-7s  %-4d  %-9s  %-6s  %s",
                renderAge(b.startTime, now),
                b.subscriberCount,
                renderBytes(b.logBytes),
                b.rooted ? "yes" : "no",
                b.key.resolvedDrv);
    }
};

static auto rCmdActiveBuilds = registerCommand2<CmdActiveBuilds>({"store", "active-builds"});

} // namespace nix
