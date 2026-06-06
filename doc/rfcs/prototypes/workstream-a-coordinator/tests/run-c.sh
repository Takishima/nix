#!/usr/bin/env bash
# Workstream C — refcounted-cancel matrix (Blocker 2, coordinator-internal).
# THROW-AWAY CODE. No wire change; the table must be agreed pre-Phase-3.
cd "$(dirname "$0")"
chmod +x ../slow-builder.sh ./*.sh 2>/dev/null
echo "==================================================================="
if ./c-matrix.sh; then echo "ALL WORKSTREAM-C CANCEL CASES PASSED"; exit 0
else echo ">>> c-matrix FAILED"; exit 1; fi
