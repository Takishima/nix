# battletest — Kubernetes battle-test harness for the remote-build protocol

Throw-away infrastructure to exercise the `claude/remote-builder-protocol-redesign-ILL36`
branch over **real SSH, real network, real concurrent clients**: live serve-2.9 log
streaming, `nix log --store ssh://`, build dedup/cancellation via the coordinator,
elastic (`self-scheduled`) builders, `use-ssh-ng-for-remote-builds`, OOM
classification, and graceful degradation against a stock upstream builder.

Results and anomalies: see [REPORT.md](REPORT.md) — that file is the product.

## Quickstart (local, kind)

Prereqs: `docker`, `kind`, `kubectl`, `nix` (flakes enabled) on the host.

```sh
./local-up.sh      # builds .#dockerImage from this branch, creates the cluster, deploys
./run-tests.sh     # runs the matrix, prints a PASS/FAIL table
./local-down.sh    # deletes the cluster
```

All three are idempotent. `run-tests.sh` accepts test numbers
(e.g. `./run-tests.sh 5` for just the dedup tests); artifacts (client/builder
logs per test) land in `.state/results-<RUN>/`.

## Topology

One container image, built from this branch's `docker.nix` via the
`.#dockerImage` flake output (nix + sshd + coreutils), used for both roles —
role config is mounted via ConfigMap/Secret, see `conf/`:

| pod | image | role |
|---|---|---|
| `builder-0`, `builder-1` (`<pod>.builders` DNS) | branch nix | sshd builders, both features on |
| `builder-oom-0` | branch nix | builder with a 256Mi memory limit (OOM test) |
| `builder-stock-0` | `nixos/nix:latest` | stock upstream builder (negative control) |
| `client-0`, `client-1` | branch nix | nix clients (`/etc/nix/machines` → builders) |

Both peers run with
`experimental-features = nix-command flakes serve-build-logs build-coordinator`
(except the stock builder). Builds inside pods use `sandbox = false`. Host keys
are shared via a Secret and pre-seeded into the clients' `known_hosts`
(plus `StrictHostKeyChecking accept-new` as fallback).

The serve (`ssh://`) and daemon (`ssh-ng://`) paths are plain
non-interactive SSH commands; the builder sshd sets `SetEnv PATH=...` and the
entrypoint symlinks the nix tools into `/usr/bin` so forced commands resolve.

## The test matrix (`run-tests.sh`)

1. `ssh://` build via the hook; failing drv shows the remote log tail inline (fail-loud).
2. Live log streaming mid-build, over both `ssh://` and `ssh-ng://`.
3. `nix log --store ssh://builder-0.builders <drv>` returns the real log after the fact.
4. `--eval-store /tmp/bt-eval --store ssh://builder-0.builders`: builds, no-op re-run,
   `nix store cat` on the remote output.
5. Dedup: two clients, same uncached slow drv, one builder → one execution
   (same `token:$$` in both logs), `nix store active-builds` shows 2 subscribers;
   then refcounted cancel (kill one client → build survives; kill both → cancelled).
6. `self-scheduled` builder (machines column 9 = `true`): two concurrent jobs on a
   `maxJobs=1` machine; control round asserts the non-elastic cap still holds.
7. `use-ssh-ng-for-remote-builds = true` + schemeless machine entry →
   `nix-daemon --stdio` observed on the builder.
8. OOM: 256Mi-limited builder, memory-ballooning drv → SIGKILL/137 failure;
   client-side rendering captured in the report.
9. Stock builder: build works over the old protocol; `nix log --store ssh://`
   fails cleanly (no hang, no protocol error).

## Hetzner (phase 3)

```sh
export HCLOUD_TOKEN=...        # or HCLOAK; never committed
cd hetzner && ./hetzner-up.sh  # hetzner-k3s: 1x cx32 master + 2x cx32 workers
KUBECONFIG=$PWD/../.state/kubeconfig-hetzner ../run-tests.sh
./hetzner-down.sh              # deletes servers/network/firewall; no LBs/volumes exist
```

Expected cost: 3× cx32 ≈ **€0.04/hour** (≈ €1/day) plus negligible traffic. The
image is `scp`'d to each node and imported with `k3s ctr images import` (no
registry needed). TODO (stretch): ARM worker pool (`cax21`) for a multi-arch
builder.

## Sandbox quirks handled by `local-up.sh`

On constrained/nested hosts (CI sandboxes) it transparently:

- mounts missing cgroup-v1 hierarchies (`name=systemd`, `cpuset`, `hugetlb`)
  that kind nodes need on hybrid-cgroup hosts;
- detects hosts that deny lowering `oom_score_adj` (which kills every
  kubelet-created pod inside kind with `runc ... can't get final child's PID
  from pipe: EOF`) and switches to a patched node image (`kind-node/`) whose
  runc wrapper strips negative `oomScoreAdj` from bundle configs.

Both are no-ops on a normal dev machine.
