#include "nix/store/build/build-registry.hh"
#include "nix/util/fmt.hh"

#include <algorithm>
#include <ctime>
#include <deque>
#include <limits>
#include <map>

namespace nix {

namespace {

/**
 * The bounded replay buffer: a ~`headCap` head + ~`tailCap` tail
 * with an explicit truncation marker once the tail evicts anything. The head
 * preserves configure/early-failure context; the tail preserves the live edge.
 */
class ReplayBuffer
{
    size_t headCap, tailCap;
    std::vector<std::string> head;
    std::deque<std::string> tail;
    size_t headBytes = 0, tailBytes = 0;
    uint64_t droppedFrames = 0, droppedBytes = 0;

public:
    explicit ReplayBuffer(ReplayBufferCaps c)
        : headCap(c.headCap)
        , tailCap(c.tailCap)
    {
    }

    void append(std::string_view frame)
    {
        if (headBytes < headCap) {
            head.emplace_back(frame);
            headBytes += frame.size();
            return;
        }
        tail.emplace_back(frame);
        tailBytes += frame.size();
        while (tailBytes > tailCap && !tail.empty()) {
            droppedBytes += tail.front().size();
            tailBytes -= tail.front().size();
            ++droppedFrames;
            tail.pop_front();
        }
    }

    /** The frames a late joiner replays: head, then a truncation marker if
     *  anything was evicted, then tail — all flagged `replayed = true`. */
    std::vector<BuildLogFrame> snapshot() const
    {
        std::vector<BuildLogFrame> out;
        out.reserve(head.size() + tail.size() + 1);
        for (auto & s : head)
            out.push_back(BuildLogFrame{s, true});
        if (droppedFrames > 0)
            out.push_back(BuildLogFrame{
                fmt("…%d frames / %d bytes truncated…\n", droppedFrames, droppedBytes), true});
        for (auto & s : tail)
            out.push_back(BuildLogFrame{s, true});
        return out;
    }
};

struct Subscriber
{
    BuildLogSink liveSink;
    BuildResultSink resultSink;
    bool keepFailed = false;
    bool deduplicated = false;
    std::optional<time_t> deadline;
};

struct Build
{
    BuildRegistryKey key;
    uint64_t epoch = 0;
    time_t startTime = 0;
    /** Sticky: set once any subscriber registers a durable root, keeps the build
     *  alive past refcount 0 (`hasRootReasonToContinue`). */
    bool rooted = false;
    uint64_t logBytes = 0;
    ReplayBuffer replay;
    std::function<void()> onCancel;
    std::map<uint64_t, Subscriber> subscribers;

    Build(BuildRegistryKey key, uint64_t epoch, ReplayBufferCaps caps, std::function<void()> onCancel)
        : key(std::move(key))
        , epoch(epoch)
        , startTime(::time(nullptr))
        , replay(caps)
        , onCancel(std::move(onCancel))
    {
    }
};

BuildResult timedOutResult()
{
    BuildResult res;
    res.inner = BuildResult::Failure{{
        .status = BuildResult::Failure::TimedOut,
        .msg = HintFmt("build timed out"),
    }};
    return res;
}

class InMemoryBuildRegistry : public BuildRegistry
{
    const BuildAuthPolicy & policy;
    ReplayBufferCaps caps;

    std::map<BuildRegistryKey, Build> builds;
    /** subscription id → its build's key, for `unsubscribe`. */
    std::map<uint64_t, BuildRegistryKey> subIndex;
    uint64_t nextSubId = 1;
    uint64_t nextEpoch = 1;

    Build * find(const BuildRegistryKey & key)
    {
        auto it = builds.find(key);
        return it == builds.end() ? nullptr : &it->second;
    }

    const Build * find(const BuildRegistryKey & key) const
    {
        auto it = builds.find(key);
        return it == builds.end() ? nullptr : &it->second;
    }

    /** Drop a build entry and forget all its subscriptions. */
    void drop(Build & build)
    {
        for (auto & [id, _] : build.subscribers)
            subIndex.erase(id);
        builds.erase(build.key);
    }

public:
    InMemoryBuildRegistry(const BuildAuthPolicy & policy, ReplayBufferCaps caps)
        : policy(policy)
        , caps(caps)
    {
    }

    std::optional<BuildAttachment> startOrAttach(
        const BuildAuth & auth,
        const BuildRegistryKey & key,
        BuildLogSink liveSink,
        BuildResultSink resultSink,
        const SubscribeOptions & opts,
        std::function<void()> onCancel) override
    {
        // Authorize BEFORE consulting the registry, so a denial does
        // not branch on whether the build exists (no existence/timing oracle).
        if (!policy.mayBuild(auth, key))
            return std::nullopt;

        Build * build = find(key);
        bool started = false;
        bool deduplicated;

        if (!build) { // MISS — start a new build
            auto [it, _] = builds.try_emplace(key, key, nextEpoch++, caps, std::move(onCancel));
            build = &it->second;
            started = true;
            deduplicated = false;
        } else { // HIT — coalesce onto the existing build
            deduplicated = true;
        }

        if (opts.explicitRoot)
            build->rooted = true; // sticky

        auto id = nextSubId++;
        build->subscribers.emplace(
            id,
            Subscriber{
                .liveSink = liveSink,
                .resultSink = std::move(resultSink),
                .keepFailed = opts.keepFailed,
                .deduplicated = deduplicated,
                .deadline = opts.deadline,
            });
        subIndex.emplace(id, key);

        // Replay the buffered log to the late joiner, then it follows the live
        // tail (registered above). Single-threaded: no frame can arrive between
        // the snapshot and the live registration, so nothing is missed or
        // duplicated at the handover.
        if (deduplicated && opts.replayWanted && liveSink)
            for (auto & frame : build->replay.snapshot())
                liveSink(frame);

        return BuildAttachment{
            .id = SubscriptionId{id},
            .deduplicated = deduplicated,
            .started = started,
            .lease = BuildLease{build->epoch},
        };
    }

    void log(const BuildRegistryKey & key, std::string_view frame) override
    {
        Build * build = find(key);
        if (!build)
            return;
        build->logBytes += frame.size();
        build->replay.append(frame);
        BuildLogFrame live{std::string(frame), false};
        for (auto & [id, sub] : build->subscribers)
            if (sub.liveSink)
                sub.liveSink(live);
    }

    void finish(const BuildRegistryKey & key, BuildResult result) override
    {
        Build * build = find(key);
        if (!build) // idempotent: already dropped (e.g. cancelled)
            return;
        for (auto & [id, sub] : build->subscribers) {
            if (sub.resultSink) {
                BuildResult r = result;
                r.deduplicated = sub.deduplicated; // per-subscriber stamp
                sub.resultSink(r);
            }
        }
        // Unconditional drop: a result — a transient failure especially —
        // is never reused for later arrivals.
        drop(*build);
    }

    bool keepFailedRequested(const BuildRegistryKey & key) const override
    {
        const Build * build = find(key);
        if (!build)
            return false;
        return std::any_of(
            build->subscribers.begin(), build->subscribers.end(), [](auto & e) { return e.second.keepFailed; });
    }

    std::optional<time_t> currentDeadline(const BuildRegistryKey & key) const override
    {
        const Build * build = find(key);
        if (!build || build->subscribers.empty())
            return std::nullopt;
        time_t max = std::numeric_limits<time_t>::min();
        for (auto & [id, sub] : build->subscribers) {
            if (!sub.deadline) // an unbounded subscriber ⇒ the envelope is unbounded
                return std::nullopt;
            max = std::max(max, *sub.deadline);
        }
        return max;
    }

    void unsubscribe(SubscriptionId id, DetachReason) override
    {
        auto idxIt = subIndex.find(id.value);
        if (idxIt == subIndex.end())
            return;
        Build * build = find(idxIt->second);
        subIndex.erase(idxIt);
        if (!build)
            return;
        build->subscribers.erase(id.value);
        // Refcounted cancellation: cancel only when no subscriber
        // remains and no durable root keeps it alive.
        if (build->subscribers.empty() && !build->rooted) {
            auto onCancel = build->onCancel;
            drop(*build);
            if (onCancel)
                onCancel();
        }
    }

    void checkDeadlines(time_t now) override
    {
        std::vector<BuildRegistryKey> toCancel;
        for (auto & [key, build] : builds) {
            std::vector<uint64_t> expired;
            for (auto & [id, sub] : build.subscribers)
                if (sub.deadline && *sub.deadline <= now)
                    expired.push_back(id);
            for (auto id : expired) {
                auto & sub = build.subscribers.at(id);
                if (sub.resultSink)
                    sub.resultSink(timedOutResult()); // per-subscriber TimedOut, detach
                subIndex.erase(id);
                build.subscribers.erase(id);
            }
            if (build.subscribers.empty() && !build.rooted)
                toCancel.push_back(key);
        }
        for (auto & key : toCancel) {
            Build * build = find(key);
            if (!build)
                continue;
            auto onCancel = build->onCancel;
            drop(*build);
            if (onCancel)
                onCancel();
        }
    }

    std::vector<ActiveBuildStatus> queryActive(const BuildAuth & auth) const override
    {
        std::vector<ActiveBuildStatus> out;
        for (auto & [key, build] : builds) {
            // Per-observable authorization: only builds this identity
            // could itself have requested are enumerated.
            if (!policy.mayBuild(auth, key))
                continue;
            out.push_back(ActiveBuildStatus{
                .key = key,
                .startTime = build.startTime,
                .subscriberCount = build.subscribers.size(),
                .logBytes = build.logBytes,
                .rooted = build.rooted,
            });
        }
        return out;
    }

    bool isLive(const BuildRegistryKey & key) const override
    {
        return find(key) != nullptr;
    }
};

} // namespace

std::unique_ptr<BuildRegistry> makeInMemoryBuildRegistry(const BuildAuthPolicy & policy, ReplayBufferCaps caps)
{
    return std::make_unique<InMemoryBuildRegistry>(policy, caps);
}

} // namespace nix
