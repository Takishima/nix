#!/usr/bin/env bash

# RFC `remote-build-protocol-redesign` Phase 3 (G3) — refcounted cancellation
# (decisions Blocker 2, guardrail §8.1 #3). A shared build lives by refcount: a
# subscriber detaching must NOT cancel a build other subscribers still want, and
# there is no originator privilege (the first subscriber is just subscriber #1).
#
# This exercises the headline case C-a from the cancel matrix: two clients attach
# to one coordinated build (same resolved derivation, via the build hook to an
# ssh-ng:// builder whose daemon relays to the coordinator); the ORIGINATOR is
# then killed while the joiner remains attached. The build must continue and the
# joiner must complete — proving the disconnect routed through a refcounted
# unsubscribe, not an unconditional "client disconnect ⇒ kill the build".

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/coord."* || true

marker="build-dedup-cancel-$$"
# Shell-only busybox (no `sleep`): hold the build in-flight with a builtin loop,
# long enough to attach the joiner and kill the originator mid-build.
iters=6000000

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord.sock"

builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

buildOne() { # $1 = local store, $2 = output file
    nix build -L -f build-dedup-coordinator.nix slow \
        --arg busybox "$busybox" \
        --argstr marker "$marker" --argstr iters "$iters" \
        --no-link --max-jobs 0 \
        --store "$1" \
        --builders "$builder" > "$2" 2>&1
}

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

outA="$TEST_ROOT/cancel-A.out"
outB="$TEST_ROOT/cancel-B.out"

# 1. Originator starts the build and holds it in-flight.
buildOne "$TEST_ROOT/machine0a" "$outA" &
pidA=$!
waitFor "grepQuiet '$marker' '$outA'" 60 \
    || { echo "originator never started" >&2; cat "$outA" >&2; kill "$pidA" 2>/dev/null; exit 1; }

# 2. Joiner attaches to the same build (replayed marker confirms the attach).
buildOne "$TEST_ROOT/machine0b" "$outB" &
pidB=$!
waitFor "grepQuiet '$marker' '$outB'" 60 \
    || { echo "joiner never attached" >&2; cat "$outB" >&2; kill "$pidA" "$pidB" 2>/dev/null; exit 1; }

# 3. Kill the ORIGINATOR while the joiner is still attached. With no originator
#    privilege, the refcount stays ≥1 and the build continues.
kill "$pidA" 2>/dev/null
wait "$pidA" 2>/dev/null || true # originator is gone; its exit status is irrelevant

# 4. The joiner must still complete successfully — the build was NOT cancelled.
wait "$pidB"; rB=$?
[ "$rB" -eq 0 ] || { echo "joiner failed after originator was killed (build wrongly cancelled?)" >&2; cat "$outB" >&2; exit 1; }

# 5. The joiner observed the shared build's token (it really attached, not a
#    fresh build of its own).
grepQuiet 'BUILDTOKEN:' "$outB"

echo "refcounted-cancel C-a OK: originator killed, joiner completed (build continued)" >&2
