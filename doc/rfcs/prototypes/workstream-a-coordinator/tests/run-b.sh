#!/usr/bin/env bash
# Workstream B — CA key-merge trust validation (F-WIRE precondition).
# THROW-AWAY CODE. T1-T3 are the literal F-WIRE precondition (validation.md).
cd "$(dirname "$0")"
chmod +x ../slow-builder.sh ./*.sh 2>/dev/null

TESTS="b-resolve b-merge b-existence-oracle b-spoof"
RC=0
for t in $TESTS; do
    echo "==================================================================="
    if ! "./$t.sh"; then RC=1; echo ">>> $t FAILED"; fi
done
echo "==================================================================="
[ "$RC" -eq 0 ] && echo "ALL WORKSTREAM-B TRUST TESTS PASSED" || echo "SOME TRUST TESTS FAILED"
exit $RC
