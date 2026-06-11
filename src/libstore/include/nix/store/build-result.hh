#pragma once
///@file

#include <string>
#include <chrono>
#include <optional>

#include "nix/store/derived-path.hh"
#include "nix/store/realisation.hh"
#include "nix/util/error.hh"
#include "nix/util/fmt.hh"
#include "nix/util/json-impls.hh"

namespace nix {

/**
 * Names must be disjoint with `BuildResultFailureStatus`.
 *
 * @note Prefer using `BuildResult::Success::Status`, this name is just
 * for sake of forward declarations.
 */
enum struct BuildResultSuccessStatus : uint8_t {
    Built,
    Substituted,
    AlreadyValid,
    ResolvesToAlreadyValid,
};

/**
 * Names must be disjoint with `BuildResultSuccessStatus`.
 *
 * @note Prefer using `BuildResult::Failure::Status`, this name is just
 * for sake of forward declarations.
 */
enum struct BuildResultFailureStatus : uint8_t {
    PermanentFailure,
    InputRejected,
    OutputRejected,
    /// possibly transient
    TransientFailure,
    /// no longer used
    CachedFailure,
    TimedOut,
    MiscFailure,
    DependencyFailed,
    LogLimitExceeded,
    NotDeterministic,
    NoSubstituters,
    /// A certain type of `OutputRejected`. The protocols do not yet
    /// know about this one, so change it back to `OutputRejected`
    /// before serialization.
    HashMismatch,
};

/**
 * How a failure should be classified *for retry*, orthogonal to the exit code.
 *
 * The exit code tells you the build process died; it does not tell you whether
 * dying was the derivation's fault. A compile error and an out-of-memory kill
 * can both surface as a non-zero exit, but only the first is a property of the
 * derivation: the second is a property of *where* it ran. That distinction is
 * what a scheduler needs to decide "retry this elsewhere" vs. "this will fail
 * again anywhere", and what a durable reuse layer needs so it never caches an
 * infrastructure failure against the build key.
 */
enum struct BuildResultFailureClass : uint8_t {
    /// The build itself failed (compile error, failing test, hash mismatch).
    /// The default — an unclassified failure is treated as build-intrinsic, so
    /// a peer that does not set this field behaves exactly as before.
    BuildError,
    /// The builder ran out of a resource (memory, disk) — e.g. an OOM kill.
    ResourceExhausted,
    /// The builder node/pod was evicted or reclaimed mid-build.
    Evicted,
    /// Any other builder-internal / infrastructural failure (network, etc.).
    Infra,
};

/**
 * Denotes a permanent build failure.
 *
 * This is both an exception type (inherits from Error) and serves as
 * the failure variant in BuildResult::inner.
 */
class BuildError : public CloneableError<BuildError, Error>
{
    void anchor() override;

public:
    using Status = BuildResultFailureStatus;
    using enum Status;

    Status status = MiscFailure;

    /**
     * If timesBuilt > 1, whether some builds did not produce the same
     * result. (Note that 'isNonDeterministic = false' does not mean
     * the build is deterministic, just that we don't have evidence of
     * non-determinism.)
     */
    bool isNonDeterministic = false;

public:
    /**
     * Variadic constructor for throwing with format strings.
     * Delegates to the string constructor after formatting.
     */
    template<typename... Args>
    BuildError(Status status, const Args &... args)
        : CloneableError(args...)
        , status{status}
    {
    }

    struct Args
    {
        Status status;
        HintFmt msg;
        bool isNonDeterministic = false;
    };

    /**
     * Constructor taking a pre-formatted error message.
     * Also used for deserialization.
     */
    BuildError(Args args)
        : CloneableError(std::move(args.msg))
        , status{args.status}
        , isNonDeterministic{args.isNonDeterministic}

    {
    }

    /**
     * Default constructor for deserialization.
     */
    BuildError()
        : CloneableError("")
    {
    }

    bool operator==(const BuildError &) const noexcept;
    std::strong_ordering operator<=>(const BuildError &) const noexcept;
};

struct BuildResult
{
    struct Success
    {
        using Status = enum BuildResultSuccessStatus;
        using enum Status;
        Status status;

        /**
         * For derivations, a mapping from the names of the wanted outputs
         * to actual paths.
         */
        SingleDrvOutputs builtOutputs;

        bool operator==(const BuildResult::Success &) const noexcept;
        std::strong_ordering operator<=>(const BuildResult::Success &) const noexcept;
    };

    /**
     * Failure is now an alias for BuildError.
     */
    using Failure = BuildError;

    std::variant<Success, Failure> inner = Failure{};

    /**
     * Convenience wrapper to avoid a longer `std::get_if` usage by the
     * caller (which will have to add more `BuildResult::` than we do
     * below also, do note.)
     */
    auto * tryGetSuccess(this auto & self)
    {
        return std::get_if<Success>(&self.inner);
    }

    /**
     * Convenience wrapper to avoid a longer `std::get_if` usage by the
     * caller (which will have to add more `BuildResult::` than we do
     * below also, do note.)
     */
    auto * tryGetFailure(this auto & self)
    {
        return std::get_if<Failure>(&self.inner);
    }

    /**
     * Throw the build error if this result represents a failure.
     * Optionally set the exit status on the error before throwing.
     */
    void tryThrowBuildError(std::optional<unsigned int> exitStatus = std::nullopt)
    {
        if (auto * failure = tryGetFailure()) {
            if (exitStatus)
                failure->withExitStatus(*exitStatus);
            throw *failure;
        }
    }

    /**
     * How many times this build was performed.
     */
    unsigned int timesBuilt = 0;

    /**
     * The start/stop times of the build (or one of the rounds, if it
     * was repeated).
     */
    time_t startTime = 0, stopTime = 0;

    /**
     * User and system CPU time the build took.
     */
    std::optional<std::chrono::microseconds> cpuUser, cpuSystem;

    /**
     * Structured build diagnostics, carried only on the gated serve 2.9 /
     * worker `build-log-query` wires. **Not frozen**: the layout may still
     * change until `SERVE_PROTOCOL_VERSION` is bumped to 2.9. An empty
     * string / zero means "absent".
     */

    /**
     * The derivation path under which the builder persisted the build log
     * (the key to pass to `nix log --store <builder>`).
     */
    std::string logRef;

    /**
     * For a failure, the build phase that failed (e.g. `"build"`), if known.
     */
    std::string failurePhase;

    /**
     * For a failure, the builder's exit status, if it ran (0 if not
     * applicable).
     */
    int64_t exitCode = 0;

    /**
     * For a failure, the tail of the build log (most recent lines), so a
     * remote failure can be rendered inline without a second round-trip.
     */
    std::string logTail;

    /**
     * Dedup/fleet-observability set, serialized after the diagnostic core
     * under the same unstable gate.
     */

    /**
     * Whether this session *attached to* an existing build of the same
     * resolved derivation rather than starting one. Stamped per-subscriber
     * by the Build Registry.
     */
    bool deduplicated = false;

    /**
     * Opaque identifier of the builder that produced this result (which
     * node/pod in a fleet). Empty when unknown; purely informational.
     */
    std::string builderId;

    using FailureClass = BuildResultFailureClass;

    /**
     * For a failure, its retry classification: anything other than
     * `BuildError` is builder-internal, so a retry layer may re-run it and
     * a durable reuse layer must not cache it against the build key.
     */
    FailureClass failureClass = FailureClass::BuildError;

    /**
     * Whether the builder was killed for exceeding memory (e.g. the kernel
     * OOM killer), when known; lets a scheduler right-size a retry.
     */
    bool killedForMemory = false;

    /**
     * Peak memory use in bytes, from the build's cgroup where one is used
     * (0 = unknown). Meaningful for successful builds too.
     */
    uint64_t peakMemoryBytes = 0;

    /**
     * Whether this failure is builder-internal rather than build-intrinsic
     * — the signal a retry layer keys on.
     */
    bool failureIsTransient() const
    {
        return failureClass != FailureClass::BuildError;
    }

    bool operator==(const BuildResult &) const noexcept;
    std::strong_ordering operator<=>(const BuildResult &) const noexcept;
};

/**
 * A `BuildResult` together with its "primary key".
 */
struct KeyedBuildResult : BuildResult
{
    /**
     * The derivation we built or the store path we substituted.
     */
    DerivedPath path;

    // Hack to work around a gcc "may be used uninitialized" warning.
    KeyedBuildResult(BuildResult res, DerivedPath path)
        : BuildResult(std::move(res))
        , path(std::move(path))
    {
    }
};

/**
 * Flags tracking different types of build failures for exit status computation.
 */
struct ExitStatusFlags
{
    /**
     * Set if at least one derivation had a BuildError (i.e. permanent
     * failure).
     */
    bool permanentFailure = false;

    /**
     * Set if at least one derivation had a timeout.
     */
    bool timedOut = false;

    /**
     * Set if at least one derivation fails with a hash mismatch.
     */
    bool hashMismatch = false;

    /**
     * Set if at least one derivation is not deterministic in check mode.
     */
    bool checkMismatch = false;

    /**
     * Update flags based on a build failure status.
     */
    void updateFromStatus(BuildResult::Failure::Status status);

    /**
     * The exit status in case of failure.
     *
     * In the case of a build failure, returned value follows this
     * bitmask:
     *
     * ```
     * 0b1100100
     *      ^^^^
     *      |||`- timeout
     *      ||`-- output hash mismatch
     *      |`--- build failure
     *      `---- not deterministic
     * ```
     *
     * In other words, the failure code is at least 100 (0b1100100), but
     * might also be greater.
     *
     * Otherwise (no build failure, but some other sort of failure by
     * assumption), this returned value is 1.
     */
    unsigned int failingExitStatus() const;
};

} // namespace nix

JSON_IMPL(nix::BuildResult)
JSON_IMPL(nix::KeyedBuildResult)
