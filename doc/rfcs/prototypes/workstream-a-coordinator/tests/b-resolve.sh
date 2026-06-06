#!/usr/bin/env bash
# B-resolve: the CA resolve->promote happy path (spike §3.8). A CA build sits in
# a `resolving` pre-state, a second client on the *same unresolved drv* attaches
# to the provisional entry, then both promote onto the resolved key and share one
# build (dedup through the resolve race).
. "$(dirname "$0")/lib.sh"
echo "[B-resolve] CA resolving pre-state -> promote -> shared build"
setup; trap teardown EXIT

U="ca:unres-$$"
R="ca:resolved-$$"
COUNTER="$SD/counter"

# Both clients target the SAME unresolved drv; both attach during `resolving`.
cl --ca -U "$U" -V "$R" -M 700 -t 1 -c "$COUNTER" -n 6 -s 150 >"$SD/c1.out" 2>"$SD/c1.err" & C1=$!
sleep 0.3   # join during the resolve phase
cl --ca -U "$U" -V "$R" -M 700 -t 1 -c "$COUNTER" -n 6 -s 150 >"$SD/c2.out" 2>"$SD/c2.err" & C2=$!
wait $C1 $C2

assert_eq       "exactly one build ran after the resolve race" 1 "$(cat "$COUNTER" 2>/dev/null)"
assert_contains "client1 succeeded"             "$SD/c1.err" "RESULT ok=1"
assert_contains "client2 succeeded"             "$SD/c2.err" "RESULT ok=1"
assert_contains "client2 was deduplicated"      "$SD/c2.err" "DEDUP=1"
assert_contains "both saw the resolve phase"    "$SD/c1.out" "resolving CA derivation"
assert_contains "both saw the resolved build"   "$SD/c1.out" "build done key=$R"
assert_contains "the coordinator promoted"      "$SD/daemon.err" "PROMOTE"
finish
