#!/usr/bin/env bash

# `nix store active-builds` — read-only introspection of the build coordinator.
# While a build is held in-flight on a single per-store coordinator,
# the command lists exactly that build (resolved-drv key, attached subscribers,
# log bytes); once the build finishes and the coordinator drops it, the command
# lists nothing.
#
# Reuses the dedup-coordinator scaffolding: a `--max-jobs 0` client offloads a
# slow derivation to one `ssh-ng://localhost` builder whose daemon has
# NIX_BUILD_COORDINATOR_SOCKET set, so its BuildDerivation relays to a single
# coordinator. The introspection client connects to that same socket (it reads
# NIX_BUILD_COORDINATOR_SOCKET too), so it sees the live registry.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/coord."* || true

marker="active-builds-marker-$$"
iters=6000000

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord.sock"

builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

out="$TEST_ROOT/active-builds.out"

# Before anything is built, the coordinator is not even running: an empty list.
n="$(nix store active-builds --store "$TEST_ROOT/machine1" --json | jq 'length')"
[ "$n" -eq 0 ] || { echo "expected no active builds before any build, got $n" >&2; exit 1; }

# 1. Start a build and hold it in-flight.
nix build -L -f build-dedup-coordinator.nix slow \
    --arg busybox "$busybox" \
    --argstr marker "$marker" --argstr iters "$iters" \
    --no-link --max-jobs 0 \
    --store "$TEST_ROOT/machine0a" \
    --builders "$builder" > "$out" 2>&1 &
pid=$!

# 2. Wait until the build is actually in-flight (its marker has streamed).
waitFor "grepQuiet '$marker' '$out'" 60 \
    || { echo "build never started streaming" >&2; cat "$out" >&2; exit 1; }

# 3. Introspect: the coordinator must report exactly this one build, and the
#    key must be a resolved .drv store path.
active="$TEST_ROOT/active.json"
waitFor "[ \"\$(nix store active-builds --store '$TEST_ROOT/machine1' --json | jq 'length')\" -eq 1 ]" 30 \
    || { echo "coordinator never reported the in-flight build" >&2; \
         nix store active-builds --store "$TEST_ROOT/machine1" --json >&2; exit 1; }

nix store active-builds --store "$TEST_ROOT/machine1" --json > "$active"
drv="$(jq -r '.[0].resolvedDrv' "$active")"
echo "active build key: $drv" >&2
[[ "$drv" == *.drv ]] || { echo "active build key is not a .drv path: $drv" >&2; exit 1; }
# Exactly one client is attached, so the count must be exactly 1 (not just
# >=1, which would also pass if refcounting double-counted the subscriber).
[ "$(jq -r '.[0].subscribers' "$active")" -eq 1 ] || { echo "expected exactly 1 subscriber" >&2; cat "$active" >&2; exit 1; }

# The human-readable rendering must mention the same derivation. The table is
# written to stderr (the logger, as `nix store info` does); JSON goes to stdout.
nix store active-builds --store "$TEST_ROOT/machine1" 2>&1 | grepQuiet "$drv" \
    || { echo "table output did not list the active build" >&2; exit 1; }

# 4. Let the build finish.
wait "$pid"; r=$?
[ "$r" -eq 0 ] || { echo "build failed" >&2; cat "$out" >&2; exit 1; }

# 5. Once the build completes the coordinator drops it: back to an empty list.
waitFor "[ \"\$(nix store active-builds --store '$TEST_ROOT/machine1' --json | jq 'length')\" -eq 0 ]" 30 \
    || { echo "coordinator still reports a build after completion" >&2; \
         nix store active-builds --store "$TEST_ROOT/machine1" --json >&2; exit 1; }

echo "active-builds introspection OK: listed the in-flight build, empty after completion" >&2
