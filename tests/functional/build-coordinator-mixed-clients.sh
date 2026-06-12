#!/usr/bin/env bash

# Heterogeneous clients coalescing on one derivation: a legacy `ssh://`
# client (serve protocol — the wire a stock-nix fleet client drives, whose
# builder-side server relays from the *goal*) and an `ssh-ng://` client
# (worker protocol, relayed by the *daemon's* BuildDerivation handler)
# build the same drv concurrently against one builder store.
#
# The registry coalesces on the relay's received bytes, so this pins the
# "all relayers stay byte-identical" invariant across the two relay
# entry points — on a derivation WITH inputDrvs, where each path applies
# its own inputSrcs hijack and any divergence silently splits the key
# into two executions. Asserted as: one token observed by both clients
# (each exactly once), live log on both wires, no degradation — in both
# join orders, because whoever starts the build determines which path's
# bytes seed the registry.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator
# Live log lines over the serve wire (the coordinator token must reach
# the ssh:// client through the serve 2.9 log stream).
enableFeatures serve-build-logs

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/mixed-"* || true

# Both fake-SSH subprocesses (serve and ng) inherit this and relay to the
# same per-store coordinator.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/mixed-coord.sock"

serveBuilder="ssh://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"
ngBuilder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

marker="mixed-marker-$$"
iters=6000000 # shell-only busybox: builtin loop holds the build in-flight

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

buildOne() { # $1 = client id, $2 = builders, $3 = drv name, $4 = out file
    timeout 240 nix build -L -f build-coordinator-mixed-clients.nix \
        --arg busybox "$busybox" --argstr marker "$marker" \
        --argstr iters "$iters" --argstr name "$3" \
        --no-link --max-jobs 0 \
        --store "$TEST_ROOT/machine0-$1" \
        --builders "$2" > "$4" 2>&1
}

tokenOf() { # $1 = out file
    sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$1" | head -n1
}

round() { # $1 = tag, $2 = first client's builders, $3 = second client's builders
    local tag="$1" first="$2" second="$3"
    local outF="$TEST_ROOT/mixed-$tag-first.out" outS="$TEST_ROOT/mixed-$tag-second.out"

    buildOne "a-$tag" "$first" "mixed-$tag" "$outF" &
    local pidF=$!
    waitFor "grepQuiet '$marker' '$outF'" 120 \
        || { echo "$tag: the first client never started streaming" >&2; cat "$outF" >&2; exit 1; }

    buildOne "b-$tag" "$second" "mixed-$tag" "$outS" &
    local pidS=$!

    wait "$pidF" || { echo "$tag: the first client failed" >&2; cat "$outF" >&2; exit 1; }
    wait "$pidS" || { echo "$tag: the second client failed" >&2; cat "$outS" >&2; exit 1; }

    local out
    for out in "$outF" "$outS"; do
        grepQuietInverse "without build dedup" "$out" \
            || { echo "$tag: a client degraded" >&2; cat "$out" >&2; exit 1; }
        local n
        n="$(grep -c 'BUILDTOKEN:' "$out" || true)"
        [ "$n" -eq 1 ] \
            || { echo "$tag: a client saw $n token lines (want exactly 1)" >&2; cat "$out" >&2; exit 1; }
    done

    local tokF tokS
    tokF="$(tokenOf "$outF")"
    tokS="$(tokenOf "$outS")"
    echo "$tag: first=$tokF second=$tokS" >&2
    [ "$tokF" = "$tokS" ] \
        || { echo "$tag: tokens differ: the serve and ssh-ng relayers did not coalesce (key bytes diverged?)" >&2; exit 1; }
}

# Whoever is first seeds the registry with its relayer's bytes; the other
# path must match them. Run both orders.
round serve-first "$serveBuilder" "$ngBuilder"
round ng-first "$ngBuilder" "$serveBuilder"

echo "mixed-protocol coalescing OK: serve and ssh-ng clients share one execution, both orders" >&2
