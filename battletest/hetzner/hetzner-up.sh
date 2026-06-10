#!/usr/bin/env bash
# Bring up the battletest cluster on Hetzner Cloud via hetzner-k3s. Idempotent.
#
# Token: export HCLOUD_TOKEN (or HCLOAK) — never committed.
# Cost: 3x cx32 (4 vCPU/8 GB) ~ EUR 0.04/h total (+ a few cents of traffic).
#       No load balancers or volumes are created by this setup.
#
# After this finishes:
#   export KUBECONFIG=$PWD/../.state/kubeconfig-hetzner
#   ../run-tests.sh
set -euo pipefail
cd "$(dirname "$0")"

export HCLOUD_TOKEN="${HCLOUD_TOKEN:-${HCLOAK:-}}"
[ -n "$HCLOUD_TOKEN" ] || { echo "set HCLOUD_TOKEN (or HCLOAK)"; exit 1; }

STATE=../.state
IMAGE_TAG=battletest-nix:dev
mkdir -p "$STATE"

# hetzner-k3s binary
if ! command -v hetzner-k3s >/dev/null; then
    os=$(uname -s | tr 'A-Z' 'a-z'); arch=$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
    echo ">>> downloading hetzner-k3s"
    curl -sSLo "$STATE/hetzner-k3s" \
        "https://github.com/vitobotta/hetzner-k3s/releases/latest/download/hetzner-k3s-${os}-${arch}"
    chmod +x "$STATE/hetzner-k3s"
    PATH="$(cd "$STATE" && pwd):$PATH"
fi

[ -f "$STATE/hetzner_ssh" ] || ssh-keygen -q -t ed25519 -N '' -C battletest-hetzner -f "$STATE/hetzner_ssh"

# Substitute only the token; everything else is static.
sed "s|\${HCLOUD_TOKEN}|$HCLOUD_TOKEN|" cluster-config.yaml.tmpl > "$STATE/hetzner-cluster.yaml"

echo ">>> creating cluster (idempotent; re-runs converge)"
hetzner-k3s create --config "$STATE/hetzner-cluster.yaml"

export KUBECONFIG="$STATE/kubeconfig-hetzner"

# ---------------------------------------------------------------------------
# Build the branch image and import it onto every node (no registry around).
# ---------------------------------------------------------------------------
echo ">>> building .#dockerImage"
nix build ../../#dockerImage --out-link "$STATE/dockerimage" \
    --extra-experimental-features 'nix-command flakes'

echo ">>> importing image onto all nodes"
sshopts=(-i "$STATE/hetzner_ssh" -o StrictHostKeyChecking=accept-new)
for ip in $(kubectl get nodes \
        -o jsonpath='{range .items[*]}{.status.addresses[?(@.type=="ExternalIP")].address}{"\n"}{end}'); do
    echo "    node $ip"
    scp "${sshopts[@]}" "$STATE/dockerimage/image.tar.gz" "root@$ip:/tmp/bt.tar.gz"
    ssh "${sshopts[@]}" "root@$ip" '
        set -e
        gunzip -f /tmp/bt.tar.gz
        out=$(k3s ctr images import /tmp/bt.tar)
        src=$(echo "$out" | grep -oE "docker.io/library/nix:[^ ]+" | head -1)
        [ -n "$src" ] && k3s ctr images tag --force "$src" docker.io/library/'"$IMAGE_TAG"'
        rm -f /tmp/bt.tar'
done

# ---------------------------------------------------------------------------
# Keys + workloads (same manifests as local).
# ---------------------------------------------------------------------------
if [ ! -f "$STATE/id_ed25519" ]; then
    ssh-keygen -q -t ed25519 -N '' -C battletest-client -f "$STATE/id_ed25519"
    ssh-keygen -q -t ed25519 -N '' -C battletest-host -f "$STATE/host_key"
fi

kubectl apply -f ../k8s/namespace.yaml
kubectl -n battletest create secret generic battletest-ssh \
    --from-file=id_ed25519="$STATE/id_ed25519" \
    --from-file=id_ed25519.pub="$STATE/id_ed25519.pub" \
    --from-file=host_key="$STATE/host_key" \
    --from-file=host_key.pub="$STATE/host_key.pub" \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl -n battletest create configmap battletest-conf --from-file=../conf/ \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl -n battletest create configmap battletest-tests --from-file=../tests/ \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl apply -f ../k8s/builders.yaml -f ../k8s/clients.yaml

for sts in builder builder-oom builder-stock client; do
    kubectl -n battletest rollout status statefulset/"$sts" --timeout=300s
done
kubectl -n battletest get pods -o wide
echo ">>> hetzner battletest cluster is up"
echo ">>> run: KUBECONFIG=$KUBECONFIG ../run-tests.sh   (from battletest/)"
