#!/usr/bin/env bash
# A-spawn: N children racing first-requests from a cold start elect EXACTLY ONE
# coordinator + one socket; the coordinator idle-exits after its grace period;
# a later request respawns it (spike §3.1 / O3).
. "$(dirname "$0")/lib.sh"
echo "[A-spawn] single-writer election, idle-exit, respawn"

export WSA_IDLE_MS=400   # short idle grace so the test can observe idle-exit
setup; trap teardown EXIT

# Fire N clients simultaneously from cold (no coordinator yet); they race to spawn.
N=8
PIDS=""
for i in $(seq 1 $N); do
    cl -k "race-$i-$$" -t 1 -c "$SD/counter-$i" -n 3 -s 150 >"$SD/r$i.out" 2>"$SD/r$i.err" &
    PIDS="$PIDS $!"
done
sleep 0.4
COORDS=$(pgrep -fc "$BINDIR/coordinator $SD"); COORDS=${COORDS:-0}
SOCKS=$(ls "$SD"/coordinator.socket 2>/dev/null | wc -l)
assert_eq "exactly one coordinator elected from $N racers" 1 "$COORDS"
assert_eq "exactly one control socket" 1 "${SOCKS:-0}"
wait $PIDS 2>/dev/null

# After all work drains, the coordinator idle-exits within its grace window.
GONE=0
for _ in $(seq 1 40); do
    C=$(pgrep -fc "$BINDIR/coordinator $SD"); C=${C:-0}
    [ "$C" -eq 0 ] && { GONE=1; break; }
    sleep 0.1
done
assert_eq "coordinator idle-exited after its grace period" 1 "$GONE"

# A fresh request respawns a coordinator (lazy spawn).
cl -k "after-idle-$$" -t 1 -c "$SD/counter-late" -n 2 -s 100 >"$SD/late.out" 2>"$SD/late.err"
assert_contains "post-idle build succeeded (coordinator respawned)" "$SD/late.err" "RESULT ok=1"
finish
