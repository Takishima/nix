#!/usr/bin/env bash

# An unusable coordinator socket location must not fail the build: the
# election throws a precise `CoordinatorUnavailable` (not 5s of blocking
# retries ending in a generic "could not reach the build coordinator"),
# and the relay degrades to an ordinary uncoordinated build with a warning
# — dedup is lost, the build is not. (The real-world shape of this is an
# unprivileged process whose coordinator path lives in a directory it
# cannot create files in.)

source common.sh

TODO_NixOS

enableFeatures build-coordinator

clearStoreIfPossible

# A socket path in a directory that does not exist (and cannot be created by
# binding): the election lock cannot even be opened.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/sub/coord.sock"

out="$(nix build -f simple.nix --no-link 2>&1)" && status=0 || status=$?
echo "$out" >&2

[ "$status" = 0 ] || { echo "build with an unusable coordinator socket failed instead of degrading" >&2; exit 1; }

# The degradation is warned about, naming the precise election failure...
echo "$out" | grepQuiet "coordinator election lock"
echo "$out" | grepQuiet "without build dedup"
# ...not the exhausted-retries fallback.
echo "$out" | grepQuietInverse "could not reach the build coordinator"
