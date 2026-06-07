#!/usr/bin/env bash
# Forward-compatibility & elastic-backend inventory tests (gate NO freeze):
#   R-class — transient vs build-intrinsic failure classification + no key
#             poisoning (RFC §4.4 / §4.7.5; deferred serve set).
#   M-arch  — same package, different system -> two distinct keys, never
#             coalesced (RFC §4.7; §8.1 guardrail #1).
# THROW-AWAY CODE. These exercise Phase-3 coordinator semantics with no
# production code yet, so they live in the prototype, not src/*-tests.
cd "$(dirname "$0")"
chmod +x ../slow-builder.sh ./*.sh 2>/dev/null
echo "==================================================================="
rc=0
./r-class.sh || rc=1
echo "-------------------------------------------------------------------"
./m-arch.sh  || rc=1
echo "==================================================================="
if [ "$rc" -eq 0 ]; then echo "ALL FORWARD-COMPAT INVENTORY TESTS PASSED"; else echo ">>> FORWARD-COMPAT TESTS FAILED"; fi
exit $rc
