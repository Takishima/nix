#!/usr/bin/env bash

# The serve (`ssh://`) live log stream must survive the builder relaying the
# build to the coordinator. (Battletest regression: with both features on,
# serve 2.9 clients received zero log lines — the relay's `log(lvlInfo)`
# frames were dropped by the serve tunnel at `verbosity = lvlError`.)
# `build-remote-serve-log-stream.sh` covers the coordinator-less serve build.

source common.sh

enableFeatures serve-build-logs
enableFeatures build-coordinator

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/coord-serve."* || true

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord-serve.sock"

# `--max-jobs 0` routes the build through the build hook; the single `ssh://`
# builder exercises the legacy serve path, whose server relays to the
# coordinator.
builder="ssh://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

out="$(nix build -L -f serve-log-stream.nix withLog \
    --arg busybox "$busybox" \
    --no-link --max-jobs 0 \
    --store "$TEST_ROOT/machine0" \
    --builders "$builder" 2>&1)"

echo "$out" >&2
echo "$out" | grepQuiet "serve-stream-marker-line"
# The coordinator path must actually be exercised: if it silently degraded to
# an uncoordinated build the daemon warns "building '…' without build dedup",
# and this test would otherwise still pass (near-tautological). Fail loudly.
echo "$out" | grepQuietInverse "without build dedup"
