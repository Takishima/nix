#!/usr/bin/env bash
# A-throughput: MEASUREMENT, not pass/fail (spike §2.3 / O4). Drive many parallel
# builds, each with several subscribers, and record the single-threaded
# coordinator's CPU and wall time -- the data that informs whether the v1 single
# event loop suffices or needs sharding (the natural axis being the build key).
. "$(dirname "$0")/lib.sh"
echo "[A-throughput] measurement of the single-threaded coordinator under load"
setup; trap teardown EXIT

BUILDS=${WSA_TP_BUILDS:-32}
SUBS=${WSA_TP_SUBS:-4}
LINES=${WSA_TP_LINES:-40}

START=$(date +%s.%N)
PIDS=""
for b in $(seq 1 "$BUILDS"); do
    KEY="tp-$b-$$"
    for s in $(seq 1 "$SUBS"); do
        cl -k "$KEY" -t 1 -c "$SD/counter-$b" -n "$LINES" -s 5 >/dev/null 2>&1 &
        PIDS="$PIDS $!"
    done
done
# Sample coordinator CPU while it runs.
sleep 0.5
COORD_PID=$(pgrep -f "$BINDIR/coordinator $SD" | head -1)
wait $PIDS 2>/dev/null
END=$(date +%s.%N)

# How many distinct builds actually ran (should equal BUILDS; dedup within each).
RAN=0
for b in $(seq 1 "$BUILDS"); do
    [ "$(cat "$SD/counter-$b" 2>/dev/null || echo 0)" = "1" ] && RAN=$((RAN+1))
done

WALL=$(awk "BEGIN { printf \"%.2f\", $END - $START }")
echo "  MEASURE builds=$BUILDS subscribers_each=$SUBS lines=$LINES"
echo "  MEASURE distinct_builds_ran=$RAN/$BUILDS (each deduped across $SUBS subs)"
echo "  MEASURE wall_seconds=$WALL"
if [ -n "${COORD_PID:-}" ] && [ -r "/proc/$COORD_PID/stat" ]; then
    read -r _ _ _ _ _ _ _ _ _ _ _ _ _ utime stime _ < "/proc/$COORD_PID/stat"
    HZ=$(getconf CLK_TCK)
    echo "  MEASURE coordinator_cpu_seconds=$(awk "BEGIN { printf \"%.2f\", ($utime + $stime)/$HZ }") (utime+stime)"
fi
# Sanity gate so the measurement run still fails loudly if coordination broke.
assert_eq "all $BUILDS builds deduped to one build each" "$BUILDS" "$RAN"
finish
