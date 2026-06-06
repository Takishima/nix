#!/bin/sh
# Workstream A prototype — the deliberately-slow test builder (deliverable A3).
#
# THROW-AWAY CODE. Stands in for "a test derivation with a deliberately slow
# builder ... that emits known marker lines and increments a counter file"
# (spike §4.1.3 / validation A3). Two side effects make the acceptance criteria
# checkable from the outside:
#
#   $COUNTER         incremented exactly ONCE per real build invocation.
#                    With dedup, only one builder runs -> counter == 1 (A-dedup).
#   $COUNTER.progress holds the latest emitted line number, so a test can see the
#                    builder still advancing even while a client is stalled
#                    (A-backpressure) or cancelled partway (refcount cancel).
#
# Args: COUNTER  KEY  NLINES  SLEEP_MS
set -eu

COUNTER="$1"
KEY="$2"
NLINES="${3:-6}"
SLEEP_MS="${4:-300}"

# Atomically bump the build counter (proof that exactly one build ran).
# A trivial lock via mkdir keeps concurrent increments correct.
lock="${COUNTER}.lock.d"
while ! mkdir "$lock" 2>/dev/null; do :; done
n=0
[ -f "$COUNTER" ] && n=$(cat "$COUNTER")
n=$((n + 1))
printf '%s' "$n" > "$COUNTER"
rmdir "$lock"

echo "=== build start key=$KEY pid=$$ (build #$n) ==="

i=1
while [ "$i" -le "$NLINES" ]; do
    echo "marker line $i/$NLINES key=$KEY"
    printf '%s' "$i" > "${COUNTER}.progress"
    # sub-second sleep so a second client reliably attaches mid-build
    if [ "$SLEEP_MS" -gt 0 ]; then
        sleep "$(awk "BEGIN { printf \"%.3f\", $SLEEP_MS/1000 }")"
    fi
    i=$((i + 1))
done

echo "=== build done key=$KEY ==="
