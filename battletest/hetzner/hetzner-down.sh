#!/usr/bin/env bash
# Tear down the Hetzner cluster COMPLETELY (servers, network, firewall, ssh
# key resource). This setup creates no load balancers (single master) and no
# volumes (no PVCs), so nothing else should be left billing. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"

export HCLOUD_TOKEN="${HCLOUD_TOKEN:-${HCLOAK:-}}"
[ -n "$HCLOUD_TOKEN" ] || { echo "set HCLOUD_TOKEN (or HCLOAK)"; exit 1; }

STATE=../.state
PATH="$(cd "$STATE" 2>/dev/null && pwd):$PATH"

if [ ! -f "$STATE/hetzner-cluster.yaml" ]; then
    sed "s|\${HCLOUD_TOKEN}|$HCLOUD_TOKEN|" cluster-config.yaml.tmpl > "$STATE/hetzner-cluster.yaml"
fi

hetzner-k3s delete --config "$STATE/hetzner-cluster.yaml" || true
rm -f "$STATE/kubeconfig-hetzner"

# Belt and braces: if the hcloud CLI is available, show anything that survived.
if command -v hcloud >/dev/null; then
    echo ">>> leftovers check (should all be empty):"
    hcloud server list 2>/dev/null | grep -i battletest || true
    hcloud load-balancer list 2>/dev/null | grep -i battletest || true
    hcloud volume list 2>/dev/null | grep -i battletest || true
else
    echo ">>> hcloud CLI not installed; double-check the Hetzner console for"
    echo "    leftover servers/LBs/volumes named nix-battletest-*"
fi
