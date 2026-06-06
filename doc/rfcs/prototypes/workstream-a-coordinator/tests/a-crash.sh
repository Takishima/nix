#!/usr/bin/env bash
# A-crash: kill the coordinator mid-build -> builders die with it (no orphan,
# PR_SET_PDEATHSIG), relay children fall back to a local build, and that
# fallback is PathLock-coalesced to EXACTLY ONE rebuild; a new build afterwards
# respawns a fresh coordinator (spike §5.1 / O2).
. "$(dirname "$0")/lib.sh"
echo "[A-crash] coordinator death -> fallback, exactly one rebuild, respawn"
setup; trap teardown EXIT

KEY=crash-$$
COUNTER="$SD/counter"

# Two clients attach to one coordinator-run build.
cl -k "$KEY" -t 1 -c "$COUNTER" -n 30 -s 200 >"$SD/c1.out" 2>"$SD/c1.err" & C1=$!
sleep 0.4
cl -k "$KEY" -t 1 -c "$COUNTER" -n 30 -s 200 >"$SD/c2.out" 2>"$SD/c2.err" & C2=$!
sleep 0.5

assert_eq "coordinator ran exactly one build before the crash" 1 "$(cat "$COUNTER" 2>/dev/null)"
COORD_PID=$(pgrep -f "$BINDIR/coordinator $SD" | head -1)
assert_gt "a coordinator was running" "${COORD_PID:-0}" 0

# Kill the coordinator mid-build.
kill -9 "$COORD_PID" 2>/dev/null
sleep 0.3
# The coordinator-forked builder must have died with it (PR_SET_PDEATHSIG): once
# the coordinator is gone, no builder whose parent is now init (reparented
# orphan) may linger. (A fresh fallback builder, parented to a live relay child,
# is legitimate and is checked separately below.)
ORPHANS=0
for p in $(pgrep -f "slow-builder.sh $COUNTER $KEY" 2>/dev/null); do
    ppid=$(awk '{print $4}' "/proc/$p/stat" 2>/dev/null || echo 0)
    [ "${ppid:-0}" = "1" ] && ORPHANS=$((ORPHANS+1))
done
assert_eq "no orphan builder survived the coordinator" 0 "${ORPHANS:-0}"

wait $C1 $C2 2>/dev/null
# Both clients still got a successful build via local fallback, and the fallback
# rebuilt exactly once (counter: 1 from the coordinator + 1 from the coalesced
# fallback == 2).
assert_contains "client1 succeeded via fallback" "$SD/c1.err" "RESULT ok=1"
assert_contains "client2 succeeded via fallback" "$SD/c2.err" "RESULT ok=1"
assert_eq "exactly one rebuild in fallback (total builds == 2)" 2 "$(cat "$COUNTER" 2>/dev/null)"

# A fresh build afterwards respawns a new coordinator (lazy spawn / O3).
cl -k "after-$$" -t 1 -c "$SD/counter2" -n 3 -s 100 >"$SD/c3.out" 2>"$SD/c3.err"
assert_contains "a new coordinator was respawned for the next build" "$SD/daemon.err" "won election"
assert_contains "post-crash build succeeded" "$SD/c3.err" "RESULT ok=1"
finish
