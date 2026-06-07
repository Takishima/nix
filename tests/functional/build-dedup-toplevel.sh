#!/usr/bin/env bash

# RFC `remote-build-protocol-redesign` Phase 3 (G3): the coordinator dedups
# *top-level* builds, not only hook-offloaded `BuildDerivation`s. Here two
# concurrent `nix build --store ssh-ng://localhost` of the same derivation send
# `BuildPathsWithResults` to the builder daemon, whose `Worker` runs the build
# through `DerivationBuildingGoal` — and that goal (with the `build-coordinator`
# feature) relays to the per-store coordinator. The two builds coalesce to one,
# the late joiner replaying the log.
#
# Same proof as build-dedup-coordinator.sh (one shared builder-process token
# observed by both clients), but driving the *general* goal-level relay path
# rather than the daemon's `BuildDerivation` op branch.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/coord."* || true

marker="build-dedup-toplevel-$$"
iters=6000000 # shell-only busybox: builtin loop holds the build in-flight

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord.sock"

# No --builders / --max-jobs 0: build directly on the ssh-ng store, so the
# builder daemon realises it via BuildPathsWithResults → Worker → the goal.
store="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1"

buildOne() { # $1 = output file
    nix build -L -f build-dedup-coordinator.nix slow \
        --arg busybox "$busybox" \
        --argstr marker "$marker" --argstr iters "$iters" \
        --no-link --store "$store" > "$1" 2>&1
}

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

outA="$TEST_ROOT/top-A.out"
outB="$TEST_ROOT/top-B.out"

buildOne "$outA" &
pidA=$!
waitFor "grepQuiet '$marker' '$outA'" 60 \
    || { echo "build A never started streaming" >&2; cat "$outA" >&2; kill "$pidA" 2>/dev/null; exit 1; }

buildOne "$outB" &
pidB=$!
waitFor "grepQuiet '$marker' '$outB'" 60 \
    || { echo "late joiner never saw the replayed marker" >&2; cat "$outB" >&2; kill "$pidA" "$pidB" 2>/dev/null; exit 1; }

wait "$pidA"; rA=$?
wait "$pidB"; rB=$?
[ "$rA" -eq 0 ] || { echo "build A failed" >&2; cat "$outA" >&2; exit 1; }
[ "$rB" -eq 0 ] || { echo "build B failed" >&2; cat "$outB" >&2; exit 1; }

tokA="$(sed -n 's/.*BUILDTOKEN:\([0-9]*\).*/\1/p' "$outA" | head -n1)"
tokB="$(sed -n 's/.*BUILDTOKEN:\([0-9]*\).*/\1/p' "$outB" | head -n1)"
echo "token A=$tokA  token B=$tokB" >&2
[ -n "$tokA" ] || { echo "no build token in A" >&2; cat "$outA" >&2; exit 1; }
[ "$tokA" = "$tokB" ] || { echo "tokens differ ⇒ two builds ran (no dedup)" >&2; exit 1; }

echo "top-level dedup OK: one build (token $tokA), two ssh-ng clients, late joiner replayed" >&2
