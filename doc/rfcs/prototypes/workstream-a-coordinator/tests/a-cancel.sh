#!/usr/bin/env bash
# Refcounted cancellation (spike §3.4, §5.2) -- the inverted MonitorFdHup.
# Covers the core of validation Workstream C's matrix that the prototype owns:
#   C-a  two attached, originator disconnects   -> build continues, #2 completes
#   C-b  one attached, disconnects, no root     -> build is cancelled
#   C-c  one attached WITH explicit root, drops -> build completes anyway
. "$(dirname "$0")/lib.sh"
echo "[A-cancel] refcounted cancellation across processes"
setup; trap teardown EXIT
COUNTER="$SD/counter"

# --- C-a: originator drops while a second client is attached -----------------
KEY=cancel-a-$$
cl -k "$KEY" -t 1 -c "$COUNTER-a" -n 10 -s 200 -b 2 >"$SD/a1.out" 2>"$SD/a1.err" & A1=$!  # disconnect-after-first
sleep 0.5
cl -k "$KEY" -t 1 -c "$COUNTER-a" -n 10 -s 200      >"$SD/a2.out" 2>"$SD/a2.err" & A2=$!  # stays attached
wait $A1 $A2
assert_contains "C-a: second client completed after originator left" "$SD/a2.err" "RESULT ok=1"
assert_eq       "C-a: progress reached completion" 10 "$(cat "$COUNTER-a.progress" 2>/dev/null)"

# --- C-b: sole client drops, no root -> build cancels ------------------------
KEY=cancel-b-$$
cl -k "$KEY" -t 1 -c "$COUNTER-b" -n 20 -s 200 -b 2 >"$SD/b1.out" 2>"$SD/b1.err" & B1=$!
wait $B1
sleep 0.8   # give the coordinator time to cancel + the builder to be killed
PROG=$(cat "$COUNTER-b.progress" 2>/dev/null || echo 0)
assert_lt "C-b: build cancelled mid-flight (progress < total)" "${PROG:-0}" 20

# --- C-c: sole client drops but holds an explicit root -> build completes -----
KEY=cancel-c-$$
cl -k "$KEY" -t 1 -c "$COUNTER-c" -n 8 -s 150 -b 2 -R 1 >"$SD/c1.out" 2>"$SD/c1.err" & CC=$!
wait $CC
sleep 1.6   # let the rooted build run to completion after the client left
assert_eq "C-c: rooted build completed despite disconnect" 8 "$(cat "$COUNTER-c.progress" 2>/dev/null)"
finish
