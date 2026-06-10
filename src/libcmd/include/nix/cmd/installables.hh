#pragma once
///@file

#include "nix/store/path.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/store/derived-path.hh"
#include "nix/cmd/built-path.hh"
#include "nix/store/store-api.hh"
#include "nix/store/build-result.hh"

#include <optional>

namespace nix {

struct PackageInfo;

enum class Realise {
    /**
     * Build the derivation.
     *
     * Postcondition: the derivation outputs exist.
     */
    Outputs,
    /**
     * Don't build the derivation.
     *
     * Postcondition: the store derivation exists.
     */
    Derivation,
    /**
     * Evaluate in dry-run mode.
     *
     * Postcondition: nothing.
     *
     * \todo currently unused, but could be revived if we can evaluate
     * derivations in-memory.
     */
    Nothing
};

/**
 * How to handle derivations in commands that operate on store paths.
 */
enum class OperateOn {
    /**
     * Operate on the output path.
     */
    Output,
    /**
     * Operate on the .drv path.
     */
    Derivation
};

/**
 * Extra info about a DerivedPath
 *
 * Yes, this is empty, but that is intended. It will be sub-classed by
 * the subclasses of Installable to allow those to provide more info.
 * Certain commands will make use of this info.
 */
struct ExtraPathInfo
{
    virtual ~ExtraPathInfo() = default;
};

/**
 * A DerivedPath with \ref ExtraPathInfo "any additional info" that
 * commands might need from the derivation.
 */
struct DerivedPathWithInfo
{
    DerivedPath path;
    ref<ExtraPathInfo> info;
};

/**
 * Like DerivedPathWithInfo but extending BuiltPath with \ref
 * ExtraPathInfo "extra info" and also possibly the \ref BuildResult
 * "result of building".
 */
struct BuiltPathWithResult
{
    BuiltPath path;
    ref<ExtraPathInfo> info;
    std::optional<BuildResult> result;
};

BuiltPaths toBuiltPaths(const std::vector<BuiltPathWithResult> & builtPathsWithResult);

/**
 * Shorthand, for less typing and helping us keep the choice of
 * collection in sync.
 */
typedef std::vector<DerivedPathWithInfo> DerivedPathsWithInfo;

struct Installable;

/**
 * Shorthand, for less typing and helping us keep the choice of
 * collection in sync.
 */
typedef std::vector<ref<Installable>> Installables;

/**
 * Installables are the main positional arguments for the Nix
 * Command-line.
 *
 * This base class is very flexible, and just assumes and the
 * Installable refers to a collection of \ref DerivedPath "derived paths" with
 * \ref ExtraPathInfo "extra info".
 */
struct Installable
{
    virtual ~Installable() {}

    /**
     * What Installable is this?
     *
     * Prints back valid CLI syntax that would result in this same
     * installable. It doesn't need to be exactly what the user wrote,
     * just something that means the same thing.
     */
    virtual std::string what() const = 0;

    /**
     * Get the collection of \ref DerivedPathWithInfo "derived paths
     * with info" that this \ref Installable instalallable denotes.
     *
     * This is the main method of this class
     */
    virtual DerivedPathsWithInfo toDerivedPaths() = 0;

    /**
     * A convenience wrapper of the above for when we expect an
     * installable to produce a single \ref DerivedPath "derived path"
     * only.
     *
     * If no or multiple \ref DerivedPath "derived paths" are produced,
     * and error is raised.
     */
    DerivedPathWithInfo toDerivedPath();

    /**
     * Return a value only if this installable is a store path or a
     * symlink to it.
     *
     * \todo should we move this to InstallableDerivedPath? It is only
     * supposed to work there anyways. Can always downcast.
     */
    virtual std::optional<StorePath> getStorePath()
    {
        return {};
    }

    static std::vector<BuiltPathWithResult> build(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        const Installables & installables,
        BuildMode bMode = bmNormal);

    static std::vector<std::pair<ref<Installable>, BuiltPathWithResult>> build2(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        const Installables & installables,
        BuildMode bMode = bmNormal);

    /**
     * The result of `buildWithFailures`: the paths that were built,
     * plus the per-derivation results of the ones that failed.
     */
    struct BuildResultsWithFailures
    {
        std::vector<BuiltPathWithResult> built;

        /**
         * The failed results, keyed by what was being built. Only
         * completed failures appear here; goals that were cancelled
         * before producing a result (e.g. without `--keep-going`)
         * do not.
         */
        std::vector<KeyedBuildResult> failures;
    };

    /**
     * Like `build`, but a failed derivation does not throw: it is
     * returned as its `KeyedBuildResult` alongside the paths that did
     * build, so the caller (e.g. `nix build --json`) can render a
     * machine-readable failure surface. The caller is responsible for
     * still failing the command, see `throwBuildErrors`.
     */
    static BuildResultsWithFailures buildWithFailures(
        ref<Store> evalStore,
        ref<Store> store,
        Realise mode,
        const Installables & installables,
        BuildMode bMode = bmNormal);

    /**
     * Throw the error(s) for the failures among `buildResults`, exactly
     * as `build` would have. No-op if there are none.
     */
    static void throwBuildErrors(std::vector<KeyedBuildResult> & buildResults, const Store & store);

    static std::set<StorePath> toStorePathSet(
        ref<Store> evalStore, ref<Store> store, Realise mode, OperateOn operateOn, const Installables & installables);

    static std::vector<StorePath> toStorePaths(
        ref<Store> evalStore, ref<Store> store, Realise mode, OperateOn operateOn, const Installables & installables);

    static StorePath toStorePath(
        ref<Store> evalStore, ref<Store> store, Realise mode, OperateOn operateOn, ref<Installable> installable);

    static std::set<StorePath>
    toDerivations(ref<Store> store, const Installables & installables, bool useDeriver = false);

    static BuiltPaths toBuiltPaths(
        ref<Store> evalStore, ref<Store> store, Realise mode, OperateOn operateOn, const Installables & installables);
};

} // namespace nix
