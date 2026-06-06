#!/usr/bin/env bash
# Workstream A prototype — run the whole acceptance-criteria harness.
# THROW-AWAY CODE. Each tests/a-*.sh exits non-zero if any assertion failed.
cd "$(dirname "$0")"
chmod +x ../slow-builder.sh ./*.sh 2>/dev/null

TESTS="a-dedup a-replay a-cancel a-backpressure a-sockauth a-crash a-spawn a-throughput"
RC=0
for t in $TESTS; do
    echo "==================================================================="
    if ! "./$t.sh"; then RC=1; echo ">>> $t FAILED"; fi
done
echo "==================================================================="
[ "$RC" -eq 0 ] && echo "ALL WORKSTREAM-A CRITERIA PASSED" || echo "SOME CRITERIA FAILED"
exit $RC
