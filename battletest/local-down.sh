#!/usr/bin/env bash
# Tear down the local battletest cluster. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"

kind delete cluster --name nix-battletest 2>/dev/null || true
echo "kind cluster deleted (docker images and battletest/.state are kept; remove .state/ to regenerate keys)"
