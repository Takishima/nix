#!/usr/bin/env bash
# A-replay: a late joiner's replayed=true prefix + live tail, concatenated,
# equals the originator's full stream -- no gap or dup at the seam (spike §3.5).
. "$(dirname "$0")/lib.sh"
echo "[A-replay] replay prefix + live tail == full stream, no gap/dup"
setup; trap teardown EXIT

KEY=replay-$$
COUNTER="$SD/counter"

cl -k "$KEY" -t 1 -c "$COUNTER" -n 10 -s 200 >"$SD/c1.out" 2>"$SD/c1.err" & C1=$!
sleep 0.9   # join well after the build has emitted several lines
cl -k "$KEY" -t 1 -c "$COUNTER" -n 10 -s 200 >"$SD/c2.out" 2>"$SD/c2.err" & C2=$!
wait $C1 $C2

# client2 must have actually replayed something (joined mid-build) ...
REPLAYED=$(sed -n 's/.*REPLAYED=\([0-9]*\).*/\1/p' "$SD/c2.err")
assert_gt "client2 received a replayed prefix" "${REPLAYED:-0}" 0
# ... and its full reconstructed stream must match the originator's byte-for-byte
assert_files_equal "replay+live == originator's full stream" "$SD/c1.out" "$SD/c2.out"
# originator replayed nothing (it was live from frame 0)
assert_contains "originator had no replay" "$SD/c1.err" "REPLAYED=0 "
finish
