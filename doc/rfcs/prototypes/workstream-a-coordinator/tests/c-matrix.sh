#!/usr/bin/env bash
# Workstream C — the full `build-dedup-cancel` matrix (decisions B2 §4, RFC §9).
# Validates the refcounted-cancel state->action table (Blocker 2 §3):
#   lifetime = refcount + explicit-root only; no originator privilege;
#   timeout  = per-subscriber detach under a max envelope;
#   keep-failed = logical OR; cancel = scoped unsubscribe-with-error.
. "$(dirname "$0")/lib.sh"
echo "[C-matrix] refcounted-cancel: C-a..C-f (Blocker 2)"
setup; trap teardown EXIT
C="$SD/counter"

# --- C-a: two attached, then kill #1 (originator) -> continue; #2 completes ----
K=ca-$$
cl -k "$K" -t 1 -c "$C-a" -n 15 -s 200 >"$SD/a1.out" 2>"$SD/a1.err" & A1=$!  # originator, stays
sleep 0.4
cl -k "$K" -t 1 -c "$C-a" -n 15 -s 200 >"$SD/a2.out" 2>"$SD/a2.err" & A2=$!  # joiner
sleep 0.5                  # both now attached
kill "$A1" 2>/dev/null     # kill the originator (passive disconnect -> UNSUBSCRIBE)
wait $A2; wait $A1 2>/dev/null
assert_contains "C-a: #2 completes after originator (#1) detaches" "$SD/a2.err" "RESULT ok=1"
assert_eq       "C-a: build ran exactly once (no originator privilege)" 1 "$(cat "$C-a" 2>/dev/null)"

# --- C-b: one attached, disconnect, no root -> cancel -------------------------
K=cb-$$
cl -k "$K" -t 1 -c "$C-b" -n 20 -s 200 -b 2 >"$SD/b1.out" 2>"$SD/b1.err" & B1=$!
wait $B1; sleep 0.8
assert_lt "C-b: build cancelled mid-flight (progress < total)" "$(cat "$C-b.progress" 2>/dev/null || echo 0)" 20

# --- C-c: one attached WITH explicit root, disconnect -> completes ------------
K=cc-$$
cl -k "$K" -t 1 -c "$C-c" -n 8 -s 150 -b 2 -R 1 >"$SD/c1.out" 2>"$SD/c1.err" & CC=$!
wait $CC; sleep 1.6
assert_eq "C-c: rooted build completes despite disconnect" 8 "$(cat "$C-c.progress" 2>/dev/null)"

# --- C-d: two timeouts (short/long) -> #1 TimedOut+detaches, #2 continues -----
K=cd-$$
cl -k "$K" -t 1 -c "$C-d" -n 25 -s 200 -T 500  >"$SD/d1.out" 2>"$SD/d1.err" & D1=$!   # short deadline
sleep 0.2
cl -k "$K" -t 1 -c "$C-d" -n 25 -s 200 -T 30000 >"$SD/d2.out" 2>"$SD/d2.err" & D2=$!   # generous deadline
wait $D1 $D2
assert_contains "C-d: short-deadline subscriber gets TimedOut" "$SD/d1.err" "status=timedout"
assert_contains "C-d: long-deadline subscriber completes"      "$SD/d2.err" "RESULT ok=1"
assert_eq       "C-d: build ran exactly once (max envelope)"   1 "$(cat "$C-d" 2>/dev/null)"

# --- C-e: build fails; --keep-failed set by exactly one of two -> dir kept -----
K=ce-$$
cl -k "$K" -t 1 -c "$C-e" -n 10 -s 150 -X 4 -F >"$SD/e1.out" 2>"$SD/e1.err" & E1=$!  # keep-failed
sleep 0.3
cl -k "$K" -t 1 -c "$C-e" -n 10 -s 150 -X 4    >"$SD/e2.out" 2>"$SD/e2.err" & E2=$!  # no keep-failed
wait $E1 $E2; sleep 0.3
assert_contains "C-e: failing build reported as failure"      "$SD/e1.err" "status=failure"
KEPT=$(ls -d "$SD"/log/*.workdir 2>/dev/null | wc -l)
assert_gt "C-e: failed dir PRESERVED (keep-failed OR)" "$KEPT" 0

# negative control: same failure, nobody asks keep-failed -> dir cleaned
K=ce2-$$
cl -k "$K" -t 1 -c "$C-e2" -n 10 -s 150 -X 4 >"$SD/e3.out" 2>"$SD/e3.err"
sleep 0.3
NEG=$(ls -d "$SD"/log/ce2-*.workdir 2>/dev/null | wc -l)
assert_eq "C-e: failed dir cleaned when nobody asked" 0 "$NEG"

# --- C-f: active-cancel by one of two -> canceller interrupted; other done ----
# #1 attaches with active-cancel intent (behavior 3 -> relay sends CANCEL_HINT on
# disconnect), reads normally until its local interrupt (SIGINT, simulating the
# client synthesizing its own interrupt); #2 stays and completes.
K=cf-$$
cl -k "$K" -t 1 -c "$C-f" -n 15 -s 200 -b 3 >"$SD/f1.out" 2>"$SD/f1.err" & F1=$!
sleep 0.4
cl -k "$K" -t 1 -c "$C-f" -n 15 -s 200      >"$SD/f2.out" 2>"$SD/f2.err" & F2=$!
sleep 0.5                  # both attached
kill -INT "$F1" 2>/dev/null   # the canceller's local interrupt
wait $F2; wait $F1 2>/dev/null
assert_contains "C-f: coordinator received a scoped CANCEL_HINT" "$SD/daemon.err" "CANCEL_HINT"
assert_contains "C-f: the other subscriber still completes"      "$SD/f2.err" "RESULT ok=1"
assert_eq       "C-f: build ran exactly once (cancel scoped to canceller)" 1 "$(cat "$C-f" 2>/dev/null)"
finish
