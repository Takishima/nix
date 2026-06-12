#!/usr/bin/env bash

# The goal-level coordinator relay must be event-loop-driven: one `nix build`
# of TWO different derivations runs one `Worker` with two goals, each relaying
# to the per-store coordinator. If the relay blocked its goal (the original
# implementation), the first relay would stall the whole Worker and the second
# build could not start until the first finished. The proof of parallelism is
# observing both builder processes alive at the same instant (their argv
# carries per-derivation markers).

source common.sh

TODO_NixOS

enableFeatures build-coordinator

clearStoreIfPossible

chmod -R +w "$TEST_ROOT/coordpar"* 2>/dev/null || true
rm -rf "$TEST_ROOT/coordpar"* || true

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coordpar.sock"

marker="coordinator-parallel-marker-$$"
# Bash busy loop: long enough to observe overlap, bounded so the test always
# terminates. ~1M iters/s in bash ⇒ ~10s in-flight window.
iters=10000000

out="$TEST_ROOT/coordpar.out"

# One invocation, one Worker, two relayed goals.
nix build -f build-coordinator-parallel.nix a b \
    --argstr marker "$marker" --argstr iters "$iters" \
    --no-link > "$out" 2>&1 &
pid=$!

# Both builders must be alive simultaneously at some point.
overlap=
for _ in $(seq 1 300); do
    if pgrep -f "$marker-A" > /dev/null && pgrep -f "$marker-B" > /dev/null; then
        overlap=1
        break
    fi
    # Bail out early if the build already finished (serialised relays finish
    # one after the other without ever overlapping).
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
done

if [ -z "$overlap" ]; then
    echo "the two relayed builds never ran concurrently (relay blocks the Worker?)" >&2
    cat "$out" >&2
    kill "$pid" 2>/dev/null
    exit 1
fi

wait "$pid" || {
    echo "the parallel relayed build failed" >&2
    cat "$out" >&2
    exit 1
}

echo "event-loop relay OK: both relayed builds were in flight concurrently" >&2
