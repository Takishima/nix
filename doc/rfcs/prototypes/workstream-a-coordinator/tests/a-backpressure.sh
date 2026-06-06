#!/usr/bin/env bash
# A-backpressure: a client that stops reading does NOT stall the builder (its
# progress keeps advancing to completion); the slow session is demoted, not the
# build (spike §3.6).
. "$(dirname "$0")/lib.sh"
echo "[A-backpressure] slow subscriber is demoted; the build is not throttled"

# Small socket buffers + a small per-session cap so the slow reader backs up into
# the coordinator's queue quickly, with a fast, modest-size build.
export WSA_SOCKBUF=4096
export WSA_OUT_CAP=8192
export WSA_CLIENT_SLEEP_MS=2000
setup; trap teardown EXIT

KEY=bp-$$
COUNTER="$SD/counter"
# behavior=1: read one record, then sleep 2s without reading (a stalled client).
# nLines large + no per-line sleep so the builder floods while the client sleeps.
cl -k "$KEY" -t 1 -c "$COUNTER" -n 4000 -s 0 -b 1 >"$SD/slow.out" 2>"$SD/slow.err" & SLOW=$!

# Meanwhile the builder must run to completion regardless of the stalled reader.
COMPLETED=0
for _ in $(seq 1 200); do
    P=$(cat "$COUNTER.progress" 2>/dev/null); P=${P:-0}
    [ "$P" -ge 4000 ] 2>/dev/null && { COMPLETED=1; break; }
    sleep 0.05
done
wait $SLOW 2>/dev/null

assert_eq       "builder ran to completion despite stalled client" 1 "$COMPLETED"
assert_contains "the slow subscriber was demoted (not the build)"  "$SD/daemon.err" "DEMOTING"
finish
