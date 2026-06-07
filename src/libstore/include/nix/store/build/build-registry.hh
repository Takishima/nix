#pragma once
///@file
///
/// The **Build Registry** (RFC `remote-build-protocol-redesign` §4.3, G3) — the
/// server-side component that coalesces concurrent realisations of the *same
/// resolved derivation* into a single build, fans that build's log out to every
/// attached session, replays the buffered log to late joiners (§4.3.1), and
/// cancels the build only when its reference count drops to zero with no durable
/// root (Blocker 2). This header is the **interface** the RFC's §4.3.5 seam 1
/// requires: operations + invariants, not a concrete data structure. The v1
/// in-memory map under a single event loop (`makeInMemoryBuildRegistry`) is one
/// conforming implementation; a persistent / sharded / distributed store is a
/// drop-in (the operations are deliberately lease/CAS-shaped, seam 2).
///
/// **Where it lives.** For single-process backends (a single `RemoteStore`
/// driver, an in-address-space service) this registry lives in process memory
/// and is the whole mechanism. For the stock fork-per-connection `nix-daemon` it
/// is hosted by the per-store **coordinator** (spike §3, decision recorded in
/// `doc/rfcs/prototypes/phase-3-coordination-decision.md`); the registry
/// interface is identical either way, which is the point of guardrail §8.1 #2
/// ("program coordination against the registry interface, not its transport").

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
 * The dedup/attach **build key**: the store path of the *resolved* derivation
 * (the derivation with every `inputDrv` replaced by its concrete output path).
 *
 * This is the load-bearing semantic commitment of §4.3 (Blocker 1, guardrail
 * §8.1 #1). It is **never** a store output path or the input-addressed `.drv`
 * path: keying on the resolved drv is what makes two distinct `.drv`s that
 * resolve identically coalesce (correct for CA derivations, harmless for
 * input-addressed ones). Because the resolved drv's hash covers `system` and
 * required features, the key is **architecture-safe** — an x86 and an aarch64
 * build of the "same" package have different resolved drvs and so never
 * coalesce.
 *
 * The registry does **not** resolve and **never** trusts a client-asserted key
 * string: the caller (coordinator / backend) computes this key by hashing the
 * resolved derivation it received and passes it in (Blocker 1).
 */
struct BuildRegistryKey
{
    /** `printStorePath(resolvedDrvPath)`. */
    std::string resolvedDrv;

    std::strong_ordering operator<=>(const BuildRegistryKey &) const = default;
    bool operator==(const BuildRegistryKey &) const = default;
};

/**
 * The authenticated identity of a session, as established by the transport
 * (peer-cred on a local coordinator socket, mTLS / pod-identity on a network
 * control plane — §4.3.4). The registry treats this as opaque identity and
 * defers the *authorization* decision to a pluggable `BuildAuthPolicy`, so no
 * call site inlines a `SO_PEERCRED` / same-host assumption (guardrail §8.1 #2).
 */
struct BuildAuth
{
    /** An opaque identity token (e.g. a uid string, or a service identity). */
    std::string identity;

    /** Whether the session is a trusted user (the existing daemon notion). */
    bool trusted = false;
};

/**
 * Pluggable authorization policy (guardrail §8.1 #2, Blocker 1). The §6
 * authorization check is the *same code* whether the registry is hosted by the
 * local coordinator or a network control plane; only the *authentication*
 * primitive behind `BuildAuth` differs.
 */
struct BuildAuthPolicy
{
    virtual ~BuildAuthPolicy() = default;

    /**
     * May this identity build/attach this resolved key? Evaluated **before**
     * the registry is consulted (Blocker 1: a denied caller learns neither the
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
 * One framed chunk of a build's structured log, as fanned out to a subscriber.
 */
struct BuildLogFrame
{
    std::string data;

    /**
     * True for frames drained from the replay buffer on attach (§4.3.1), so the
     * client can render them dimmed / collapsed; false for the live tail.
     */
    bool replayed = false;
};

/** Where a subscriber's log frames go. Must not block (backpressure / slow-client
 *  demotion is layered above, §3.6). */
using BuildLogSink = std::function<void(const BuildLogFrame &)>;

/** Where a subscriber's final `BuildResult` is delivered. */
using BuildResultSink = std::function<void(const BuildResult &)>;

/**
 * Per-subscriber options (Blocker 2). Deadlines are **per-subscriber**: the
 * shared build runs under the *maximum* (most-generous) envelope of attached
 * subscribers, and a subscriber whose own deadline elapses is detached with a
 * `TimedOut` result without cancelling the build for others (strictest-wins is
 * rejected as a cross-tenant DoS).
 */
struct SubscribeOptions
{
    /** Replay the buffered log on attach before joining the live tail. */
    bool replayWanted = true;

    /** This subscriber requested `--keep-failed`. Honoured as a logical OR over
     *  attached subscribers (Blocker 2): the failed build dir is one artifact. */
    bool keepFailed = false;

    /** A durable build/GC root ("I want this output"). This — and *only* this —
     *  is `hasRootReasonToContinue()`: it keeps the build alive past refcount 0.
     *  It is **sticky**: once any subscriber registers a root the build runs to
     *  completion even after every subscriber detaches. `--keep-going` is *not*
     *  a root (it is per-client sibling scheduling, Blocker 2). */
    bool explicitRoot = false;

    /** Absolute wall-clock deadline for *this* subscriber, if any. */
    std::optional<time_t> deadline;
};

/** Why a subscriber detached. Informational for the registry (both decrement the
 *  refcount identically); `Cancel` additionally means the client synthesized its
 *  own local interrupt (Blocker 2: cancel is a scoped unsubscribe-with-error, no
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
    explicit operator bool() const { return value != 0; }
};

/**
 * A monotonic fencing token for a key's live build (§4.3.5 seam 2). v1 gets
 * atomicity for free from its single event loop, but the operations are written
 * as if backed by compare-and-swap + a lease: a future persistent/distributed
 * registry expresses "who is building `K`, recover safely if they vanish" by
 * renewing the lease while building and **fencing** it (bumping the epoch) when
 * the build dies — without touching callers. Each (re)creation of a key bumps
 * the epoch, so a stale handle can detect it was fenced.
 */
struct BuildLease
{
    uint64_t epoch = 0;
    bool valid() const { return epoch != 0; }
    std::strong_ordering operator<=>(const BuildLease &) const = default;
};

/** The outcome of a `startOrAttach`. */
struct BuildAttachment
{
    /** Handle for the subscriber, for later `unsubscribe`. */
    SubscriptionId id;

    /**
     * Whether this session *attached to an existing* build rather than starting
     * one — i.e. it was a registry HIT. This is the `deduplicated` flag the
     * Phase 3 wire reports (gate H3); the registry stamps it onto each
     * subscriber's delivered `BuildResult`.
     */
    bool deduplicated = false;

    /** True iff this call *started* the build (a registry MISS). The caller that
     *  started it is the one that must drive it (call `log`/`finish`). */
    bool started = false;

    /** The live lease for the key (seam 2). */
    BuildLease lease;
};

/** A read-only snapshot of one in-flight build, for `QueryActiveBuilds` (G5,
 *  §4.3.2). */
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
 * The Build Registry interface (§4.3.5 seam 1).
 *
 * **Invariant:** at most one live build per key per coordination domain, with
 * `startOrAttach` (lookup-and-create) atomic with respect to the key.
 *
 * Driving model: the registry *coordinates* but does not itself run builders.
 * The session that gets `started = true` from `startOrAttach` (the MISS) drives
 * the build by calling `log()` as the builder emits frames and `finish()` when
 * it completes; the registry fans those out, manages the replay buffer, and
 * owns the refcounted lifetime. When the registry decides to cancel (refcount 0,
 * no root, or all deadlines elapsed) it invokes the MISS caller's `onCancel`.
 */
struct BuildRegistry
{
    virtual ~BuildRegistry() = default;

    /**
     * Atomically look up `key` and subscribe this session to it (Blocker 1:
     * **authorize before** consulting the registry). On a MISS the build is
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
     *  (e.g. after a cancel). */
    virtual void finish(const BuildRegistryKey & key, BuildResult result) = 0;

    /** Whether *any* currently-attached subscriber of `key` requested
     *  `--keep-failed` (Blocker 2: logical OR). The builder consults this before
     *  cleaning up a failed build dir. */
    virtual bool keepFailedRequested(const BuildRegistryKey & key) const = 0;

    /** The build's current effective deadline: the **maximum** of attached
     *  subscribers' deadlines (the most-generous envelope), or `nullopt` if any
     *  attached subscriber has no deadline. The builder arms its silent-time /
     *  build timeout from this. */
    virtual std::optional<time_t> currentDeadline(const BuildRegistryKey & key) const = 0;

    /** Detach a subscriber (refcounted cancellation, Blocker 2). Decrements the
     *  refcount; if it reaches zero and no durable root exists, the build is
     *  cancelled (its `onCancel` fires) and the entry dropped — otherwise the
     *  build continues for the remaining subscribers / to completion under a
     *  root. */
    virtual void unsubscribe(SubscriptionId id, DetachReason reason) = 0;

    /** Event-loop tick: evaluate per-subscriber deadlines against `now`. Any
     *  subscriber past its deadline receives a `TimedOut` `BuildResult` and is
     *  detached (without cancelling the build for others); if that empties the
     *  build and no root exists, the build is cancelled (timeout). */
    virtual void checkDeadlines(time_t now) = 0;

    /** Authorization-filtered enumeration of in-flight builds (§4.3.2, G5). Only
     *  builds the identity may observe are returned (Blocker 1 per-observable
     *  authz); the `QueryActiveBuilds` privacy default (O6) is the caller's. */
    virtual std::vector<ActiveBuildStatus> queryActive(const BuildAuth & auth) const = 0;

    /** Whether `key` currently has a live build (test / introspection seam). */
    virtual bool isLive(const BuildRegistryKey & key) const = 0;
};

/**
 * Replay-buffer caps (O5): a byte cap, default 4 MiB total, kept as a ~1 MiB
 * **head** + ~3 MiB **tail** with an explicit truncation marker once exceeded —
 * the head preserves configure/early-failure context, the tail the live edge a
 * late joiner is about to follow.
 */
struct ReplayBufferCaps
{
    size_t headCap = 1u << 20; // 1 MiB
    size_t tailCap = 3u << 20; // 3 MiB
};

/**
 * Construct the v1 **in-memory** Build Registry: an in-process map under a
 * single (caller-provided) event loop, with replay buffers in process memory
 * (RFC Q1, O5). This is the registry used directly by single-process backends
 * and hosted by the coordinator for the stock daemon. `policy` must outlive the
 * registry.
 */
std::unique_ptr<BuildRegistry>
makeInMemoryBuildRegistry(const BuildAuthPolicy & policy, ReplayBufferCaps caps = {});

} // namespace nix
