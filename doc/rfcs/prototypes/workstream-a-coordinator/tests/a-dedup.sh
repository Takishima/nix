#!/usr/bin/env bash
# A-dedup: two concurrent clients on the same drv -> builder_counter == 1.
# (validation.md Workstream A; mirrors RFC §9 "Dedup test")
. "$(dirname "$0")/lib.sh"
echo "[A-dedup] exactly one build runs for two concurrent clients"
setup; trap teardown EXIT

KEY=dedup-$$
COUNTER="$SD/counter"

cl -k "$KEY" -t 1 -c "$COUNTER" -n 6 -s 300 >"$SD/c1.out" 2>"$SD/c1.err" & C1=$!
sleep 0.5   # let client1 start the build
cl -k "$KEY" -t 1 -c "$COUNTER" -n 6 -s 300 >"$SD/c2.out" 2>"$SD/c2.err" & C2=$!
wait $C1 $C2

assert_eq  "exactly one build ran"            1 "$(cat "$COUNTER" 2>/dev/null)"
assert_contains "client1 is the originator"   "$SD/c1.err" "DEDUP=0"
assert_contains "client2 is deduplicated"     "$SD/c2.err" "DEDUP=1"
assert_contains "client1 build succeeded"     "$SD/c1.err" "RESULT ok=1"
assert_contains "client2 build succeeded"     "$SD/c2.err" "RESULT ok=1"
assert_files_equal "both clients saw the same full log" "$SD/c1.out" "$SD/c2.out"
finish
