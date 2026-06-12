#pragma once
///@file
///
/// The **Build Registry**: coalesces concurrent realisations of the same
/// resolved derivation into a single build, fans the log out to every
/// attached session, replays the buffered log to late joiners, and
/// cancels the build only when its refcount drops to zero with no durable
/// root. This header is the interface — operations + invariants — hosted
/// by the per-store coordinator process for the stock daemon.

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nix/store/build-result.hh"

namespace nix {

/**
 * The dedup/attach **build key**: the store path of the *resolved*
 * derivation. Keying on the resolved drv makes two distinct `.drv`s that
 * resolve identically coalesce, and is architecture-safe (the hash covers
 * `system` and required features). The caller computes it by hashing the
 * derivation it actually received — never a client-asserted string.
 */
struct BuildRegistryKey
{
    /** `printStorePath(resolvedDrvPath)`. */
    std::string resolvedDrv;

    std::strong_ordering operator<=>(const BuildRegistryKey &) const = default;
    bool operator==(const BuildRegistryKey &) const = default;
};

/**
 * The authenticated identity of a session, as established by the
 * transport; the registry treats it as opaque and defers authorization to
 * a pluggable `BuildAuthPolicy`.
 */
struct BuildAuth
{
    /** An opaque identity token (e.g. a uid string, or a service identity). */
    std::string identity;

    /** Whether the session is a trusted user (the existing daemon notion).
     *  Only meaningful when the transport authenticated it; on the local
     *  coordinator socket the peer asserts it itself, so the coordinator
     *  always passes `false` and no policy may rely on it there. */
    bool trusted = false;
};

/**
 * Pluggable authorization policy, the same code whatever the transport.
 */
struct BuildAuthPolicy
{
    virtual ~BuildAuthPolicy() = default;

    /**
     * May this identity build/attach this resolved key? Evaluated **before**
     * the registry is consulted (a denied caller learns neither the
     * log nor whether the build exists — no existence/timing oracle).
     */
    virtual bool mayBuild(const BuildAuth & auth, const BuildRegistryKey & key) const = 0;
};

/** A policy that permits everything — the single-process / single-tenant case
 *  (one trusted driver, no cross-tenant boundary). */
struct AllowAllAuthPolicy : BuildAuthPolicy
{
    bool mayBuild(const BuildAuth &, const BuildRegistryKey &) const override
    {
        return true;
    }
};

/**
 * Permits exactly one identity. The registry enforces the same boundary
 * as the transport's peer-cred gate, so a transport widened (or buggy)
 * ahead of a real multi-tenant policy fails closed instead of open.
 */
struct SingleIdentityAuthPolicy : BuildAuthPolicy
{
    std::string identity;

    explicit SingleIdentityAuthPolicy(std::string identity)
        : identity(std::move(identity))
    {
    }

    bool mayBuild(const BuildAuth & auth, const BuildRegistryKey &) const override
    {
        return auth.identity == identity;
    }
};

/**
 * One framed chunk of a build's structured log, as fanned out to a subscriber.
 */
struct BuildLogFrame
{
    std::string data;

    /**
     * True for frames drained from the replay buffer on attach, so the
     * client can render them dimmed / collapsed; false for the live tail.
     */
    bool replayed = false;
};

/** Where a subscriber's log frames go. Must not block (backpressure / slow-client
 *  demotion is layered above). */
using BuildLogSink = std::function<void(const BuildLogFrame &)>;

/** Where a subscriber's final `BuildResult` is delivered. */
using BuildResultSink = std::function<void(const BuildResult &)>;

/**
 * Per-subscriber options for a `startOrAttach`.
 */
struct SubscribeOptions
{
    /** Replay the buffered log on attach before joining the live tail. */
    bool replayWanted = true;

    /** This subscriber requested `--keep-failed`. Honoured as a logical OR over
     *  attached subscribers: the failed build dir is one artifact. */
    bool keepFailed = false;

    /** A durable build/GC root ("I want this output"). This — and *only* this —
     *  is `hasRootReasonToContinue()`: it keeps the build alive past refcount 0.
     *  It is **sticky**: once any subscriber registers a root the build runs to
     *  completion even after every subscriber detaches. `--keep-going` is *not*
     *  a root (it is per-client sibling scheduling). */
    bool explicitRoot = false;
};

/** Why a subscriber detached. Informational for the registry (both decrement the
 *  refcount identically); `Cancel` additionally means the client synthesized its
 *  own local interrupt (cancel is a scoped unsubscribe-with-error, no
 *  new wire status). */
enum class DetachReason {
    /** Client connection hung up. */
    Hup,
    /** Client actively cancelled. */
    Cancel,
};

/** Opaque per-subscriber handle returned by `startOrAttach`, used to detach. */
struct SubscriptionId
{
    uint64_t value = 0;
    std::strong_ordering operator<=>(const SubscriptionId &) const = default;
    bool operator==(const SubscriptionId &) const = default;

    explicit operator bool() const
    {
        return value != 0;
    }
};

/** The outcome of a `startOrAttach`. */
struct BuildAttachment
{
    /** Handle for the subscriber, for later `unsubscribe`. */
    SubscriptionId id;

    /**
     * Whether this session *attached to an existing* build rather than starting
     * one — i.e. it was a registry HIT. This is the `deduplicated` flag the
     * wire reports; the registry stamps it onto each
     * subscriber's delivered `BuildResult`.
     */
    bool deduplicated = false;

    /** True iff this call *started* the build (a registry MISS). The caller that
     *  started it is the one that must drive it (call `log`/`finish`). */
    bool started = false;
};

/** A read-only snapshot of one in-flight build, for `QueryActiveBuilds`. */
struct ActiveBuildStatus
{
    BuildRegistryKey key;
    time_t startTime = 0;
    /** Number of currently-attached subscribers. */
    size_t subscriberCount = 0;
    /** Bytes of structured log seen so far (un-truncated total). */
    uint64_t logBytes = 0;
    /** Whether a durable root keeps this build alive past refcount 0. */
    bool rooted = false;
};

/**
 * The Build Registry interface.
 *
 * **Invariant:** at most one live build per key per coordination domain, with
 * `startOrAttach` (lookup-and-create) atomic with respect to the key.
 *
 * Driving model: the registry *coordinates* but does not itself run builders.
 * The session that gets `started = true` from `startOrAttach` (the MISS) drives
 * the build by calling `log()` as the builder emits frames and `finish()` when
 * it completes; the registry fans those out, manages the replay buffer, and
 * owns the refcounted lifetime. When the registry decides to cancel (refcount 0
 * with no root) it invokes the MISS caller's `onCancel`.
 */
struct BuildRegistry
{
    virtual ~BuildRegistry() = default;

    /**
     * Atomically look up `key` and subscribe this session to it (**authorize
     * before** consulting the registry). On a MISS the build is
     * created and `onCancel` is recorded; on a HIT the session attaches and (if
     * `replayWanted`) the buffered log is immediately replayed to `liveSink`
     * with `replayed = true` before the live tail.
     *
     * @return `std::nullopt` iff authorization is denied — a *uniform* denial
     * that does not branch on whether the build exists (no existence oracle).
     *
     * @param onCancel Used only on a MISS: invoked when the registry decides to
     * cancel the build, so the builder can interrupt itself.
     */
    virtual std::optional<BuildAttachment> startOrAttach(
        const BuildAuth & auth,
        const BuildRegistryKey & key,
        BuildLogSink liveSink,
        BuildResultSink resultSink,
        const SubscribeOptions & opts,
        std::function<void()> onCancel = {}) = 0;

    /** Broadcast a live log frame for `key`'s build: append to the replay buffer
     *  and forward to every attached subscriber (`replayed = false`). No-op if
     *  `key` is not live. */
    virtual void log(const BuildRegistryKey & key, std::string_view frame) = 0;

    /** The build for `key` finished: deliver `result` (with each subscriber's
     *  `deduplicated` flag stamped on) to every attached subscriber, then drop
     *  the entry. Idempotent: a `finish` for an already-dropped key is a no-op
     *  (e.g. after a cancel).
     *
     *  Invariant: a result is shared only with subscribers attached to *this
     *  execution*, never retained as a canonical answer for `key`. In
     *  particular a transient failure (`result.failureIsTransient()`) must
     *  not be replayed to a later arrival, which gets a fresh build; durable
     *  result reuse must keep transient failures out of its cache. */
    virtual void finish(const BuildRegistryKey & key, BuildResult result) = 0;

    /** Whether *any* currently-attached subscriber of `key` requested
     *  `--keep-failed` (logical OR). The builder consults this before
     *  cleaning up a failed build dir. */
    virtual bool keepFailedRequested(const BuildRegistryKey & key) const = 0;

    /** Detach a subscriber (refcounted cancellation). Decrements the
     *  refcount; if it reaches zero and no durable root exists, the build is
     *  cancelled (its `onCancel` fires) and the entry dropped — otherwise the
     *  build continues for the remaining subscribers / to completion under a
     *  root. */
    virtual void unsubscribe(SubscriptionId id, DetachReason reason) = 0;

    /** Authorization-filtered enumeration of in-flight builds. Only builds the
     *  identity may observe are returned (per-observable authz); the
     *  `QueryActiveBuilds` privacy default is the caller's. */
    virtual std::vector<ActiveBuildStatus> queryActive(const BuildAuth & auth) const = 0;

    /** Whether `key` currently has a live build (for tests / introspection). */
    virtual bool isLive(const BuildRegistryKey & key) const = 0;
};

/**
 * Replay-buffer caps: a head + tail with a truncation marker once
 * exceeded — the head preserves early context, the tail the live edge a
 * late joiner is about to follow.
 */
struct ReplayBufferCaps
{
    size_t headCap = 1u << 20; // 1 MiB
    size_t tailCap = 3u << 20; // 3 MiB
};

/**
 * The in-memory Build Registry: an in-process map under a single
 * (caller-provided) event loop. `policy` must outlive the registry.
 */
std::unique_ptr<BuildRegistry> makeInMemoryBuildRegistry(const BuildAuthPolicy & policy, ReplayBufferCaps caps = {});

} // namespace nix
