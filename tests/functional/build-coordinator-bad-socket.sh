#!/usr/bin/env bash

# The relay's connect/spawn handshake is event-driven and fails fast: the
# election winner binds the coordinator socket *itself* (before spawning the
# coordinator), so there is no sleep-and-retry window in the common path —
# and a socket location that cannot work surfaces as an immediate, precise
# error instead of 5s of blocking retries ending in a generic
# "could not reach the build coordinator".

source common.sh

TODO_NixOS

enableFeatures build-coordinator

clearStoreIfPossible

# A socket path in a directory that does not exist (and cannot be created by
# binding): the election lock cannot even be opened.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/sub/coord.sock"

out="$(nix build -f simple.nix --no-link 2>&1)" && status=0 || status=$?
echo "$out" >&2

[ "$status" != 0 ] || { echo "build with an unusable coordinator socket unexpectedly succeeded" >&2; exit 1; }

# The precise election error, not the exhausted-retries fallback.
echo "$out" | grepQuiet "coordinator election lock"
echo "$out" | grepQuietInverse "could not reach the build coordinator"
