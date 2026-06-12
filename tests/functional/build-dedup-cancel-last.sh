#!/usr/bin/env bash

# Refcounted cancellation, the terminal case: when the LAST subscriber
# disconnects (refcount → 0, no durable root), the coordinator must actually
# stop the running build — not merely forget it in the registry. (Battletest
# regression: the registry entry vanished but the build child never acted on
# the cancellation SIGINT — blocked signal mask after fork — and ran to
# completion.) `build-dedup-cancel.sh` covers the complement: a detach must
# NOT cancel while other subscribers remain.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"
command -v pgrep > /dev/null || skipTest "no pgrep"

enableFeatures build-coordinator

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/coord-last."* || true

marker="build-dedup-cancel-last-$$"
# Minutes of busy loop: the build must not be able to finish naturally while
# we observe cancellation (a cancelled-too-late false PASS).
iters=600000000

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord-last.sock"

builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

out="$TEST_ROOT/cancel-last.out"

# `exec` so the backgrounded pid IS the nix client process (killing a wrapper
# subshell would leave nix alive and prove nothing).
startBuild() {
    exec nix build -L -f build-dedup-cancel-last.nix \
        --arg busybox "$busybox" \
        --argstr marker "$marker" --argstr iters "$iters" \
        --no-link --max-jobs 0 \
        --store "$TEST_ROOT/machine0" \
        --builders "$builder" > "$out" 2>&1
}

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

# The builder runs `sh -c '<script containing $marker>'`, so the live build
# process (and only it, once the client is dead) matches the marker.
buildAlive() {
    pgrep -f "$marker" > /dev/null
}

startBuild &
pid=$!

# In flight: the marker streamed back to the client's log.
waitFor "grepQuiet '$marker' '$out'" 60 \
    || { echo "build never started" >&2; cat "$out" >&2; kill "$pid" 2>/dev/null; exit 1; }

# Kill the ONLY subscriber. Refcount hits zero, nothing is rooted: the
# coordinator must stop the build child (and with it the builder process).
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null || true

if ! waitFor "! buildAlive" 30; then
    pkill -9 -f "$marker" 2>/dev/null || true # don't leave a minutes-long orphan
    echo "build survived the last subscriber's disconnect (cancel did not stop it)" >&2
    exit 1
fi

echo "refcounted-cancel-last OK: killing the last subscriber stopped the build" >&2
