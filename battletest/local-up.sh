#!/usr/bin/env bash
# Bring up the local battletest cluster (kind). Idempotent.
#
# Prereqs: docker, kind, kubectl, nix (with flakes) on the host.
# Usage: ./local-up.sh
set -euo pipefail
cd "$(dirname "$0")"

CLUSTER=nix-battletest
NODE_IMAGE_DEFAULT=kindest/node:v1.34.0
IMAGE_TAG=battletest-nix:dev
STATE=.state
mkdir -p "$STATE"

log() { echo ">>> $*"; }

# ---------------------------------------------------------------------------
# 0. Sandbox quirks (no-ops on a normal Linux/macOS dev machine).
# ---------------------------------------------------------------------------
NODE_IMAGE=$NODE_IMAGE_DEFAULT
if [ "$(uname -s)" = Linux ]; then
    # kind nodes need the name=systemd cgroup v1 hierarchy on hybrid hosts.
    if [ "$(stat -fc %T /sys/fs/cgroup 2>/dev/null)" = tmpfs ]; then
        for h in systemd cpuset hugetlb; do
            if [ ! -d "/sys/fs/cgroup/$h" ] || ! mountpoint -q "/sys/fs/cgroup/$h"; then
                log "mounting missing cgroup v1 hierarchy: $h"
                mkdir -p "/sys/fs/cgroup/$h"
                if [ "$h" = systemd ]; then
                    mount -t cgroup -o none,name=systemd cgroup "/sys/fs/cgroup/$h" || true
                else
                    mount -t cgroup -o "$h" cgroup "/sys/fs/cgroup/$h" || true
                fi
            fi
        done
    fi
    # Some sandboxes deny lowering oom_score_adj, which breaks every
    # kubelet-created pod. Detect and switch to a patched node image.
    if ! bash -c 'echo -1 > /proc/self/oom_score_adj' 2>/dev/null; then
        NODE_IMAGE=battletest-kind-node:dev
        if ! docker image inspect "$NODE_IMAGE" >/dev/null 2>&1; then
            log "host denies negative oom_score_adj; building patched kind node image"
            docker build -t "$NODE_IMAGE" --build-arg BASE="$NODE_IMAGE_DEFAULT" kind-node/
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 1. Build the container image from THIS branch's nix (flake output wraps
#    docker.nix; it already contains nix + sshd + bash + coreutils).
# ---------------------------------------------------------------------------
if [ -z "${SKIP_IMAGE_BUILD:-}" ]; then
    log "building .#dockerImage from the branch (cached after first run)"
    nix build ../#dockerImage --out-link "$STATE/dockerimage" \
        --extra-experimental-features 'nix-command flakes'
    log "loading image into docker"
    loaded=$(docker load < "$STATE/dockerimage/image.tar.gz" | tail -1)
    # "Loaded image: nix:<version>"
    src_tag=${loaded##* }
    docker tag "$src_tag" "$IMAGE_TAG"
fi
docker pull -q nixos/nix:latest || log "WARNING: could not pull nixos/nix (stock builder may not start)"

# ---------------------------------------------------------------------------
# 2. Cluster.
# ---------------------------------------------------------------------------
if ! kind get clusters 2>/dev/null | grep -qx "$CLUSTER"; then
    log "creating kind cluster $CLUSTER (node image: $NODE_IMAGE)"
    kind create cluster --name "$CLUSTER" --image "$NODE_IMAGE" --wait 180s
else
    log "kind cluster $CLUSTER already exists"
fi
kubectl config use-context "kind-$CLUSTER" >/dev/null

log "loading images into the cluster"
kind load docker-image --name "$CLUSTER" "$IMAGE_TAG"
kind load docker-image --name "$CLUSTER" nixos/nix:latest || true

# ---------------------------------------------------------------------------
# 3. Keys, config, workloads.
# ---------------------------------------------------------------------------
if [ ! -f "$STATE/id_ed25519" ]; then
    log "generating ssh keys"
    ssh-keygen -q -t ed25519 -N '' -C battletest-client -f "$STATE/id_ed25519"
    ssh-keygen -q -t ed25519 -N '' -C battletest-host -f "$STATE/host_key"
fi

kubectl apply -f k8s/namespace.yaml

kubectl -n battletest create secret generic battletest-ssh \
    --from-file=id_ed25519="$STATE/id_ed25519" \
    --from-file=id_ed25519.pub="$STATE/id_ed25519.pub" \
    --from-file=host_key="$STATE/host_key" \
    --from-file=host_key.pub="$STATE/host_key.pub" \
    --dry-run=client -o yaml | kubectl apply -f -

kubectl -n battletest create configmap battletest-conf --from-file=conf/ \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl -n battletest create configmap battletest-tests --from-file=tests/ \
    --dry-run=client -o yaml | kubectl apply -f -

kubectl apply -f k8s/builders.yaml -f k8s/clients.yaml

# Pick up config changes on re-runs (pods mount config at start).
if [ -n "${RESTART_PODS:-}" ]; then
    kubectl -n battletest rollout restart statefulset builder builder-oom builder-stock client
fi

log "waiting for rollout"
for sts in builder builder-oom builder-stock client; do
    kubectl -n battletest rollout status statefulset/"$sts" --timeout=300s
done

kubectl -n battletest get pods -o wide
log "battletest cluster is up; run ./run-tests.sh"
