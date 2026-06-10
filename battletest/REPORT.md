# battletest report — remote-build protocol branch on Kubernetes

> **Status update:** anomalies 1–4 below have since been fixed on this
> branch, each with a regression test that reproduces it first:
>
> | anomaly | fix | tests |
> |---|---|---|
> | 1 (no serve log delivery with coordinator) | `libstore: surface coordinator-relayed build logs as results` | `CoordinatorRelay.*` unit tests; `tests/functional/build-remote-serve-log-stream-coordinator.sh` |
> | 2 (cancel doesn't stop the build) | `libstore: make coordinator cancellation actually stop the build` | `tests/functional/build-dedup-cancel-last.sh` |
> | 3 (duplicate fail-loud tail) + 4 (signal→exit-code downgrade) | `nix: fix remote-failure rendering (duplicate tail, hook status)` | extended `tests/functional/build-remote-fail-loud.sh` |
>
> **Re-verified end-to-end:** the matrix was re-run on this kind cluster
> against an image built from the fixed branch
> (`2.35.0pre20260610_7424297`, run `124040`): **10/10 PASS**, including
> the two previous failures — `2-live-streaming` (log lines visible
> mid-build on `ssh://` and `ssh-ng://`) and `5b-refcount-cancel`
> (builds before=1, after-kill-one=1, after-kill-both=0).
>
> Anomalies 5 (`ResourceExhausted` not surfaced end-to-end) and 6
> (new-CLI `nix build --store ssh://` build gap) remain open — both need
> a small design decision rather than a contained patch.
> Incidental finding while writing the killed-builder test: a sandboxed
> builder is PID 1 of its pid namespace, so a *self*-sent SIGKILL is
> silently dropped by the kernel, and chroot stores force the sandbox on
> (`storeDir != realStoreDir`) even with `sandbox = false`.

Branch under test: `claude/remote-builder-protocol-redesign-ILL36`
(nix `2.35.0pre20260610_f551e7f`), negative control: `nixos/nix:latest`
(nix `2.34.7`). Real OpenSSH 10.3 between pods, kind cluster, two branch
builders + one 256Mi-limited builder + one stock builder + two clients.

Executed end-to-end inside a CI sandbox (Linux 6.18, 4 cores, docker 29.3.1
with the containerd snapshotter, hybrid cgroup v1). Phase 1 + the full matrix
**ran for real**; phase 3 (Hetzner) is implemented but **not executed** (no
`HCLOUD_TOKEN` in this environment) — scripts/manifests are dry-run-validated
only.

## Result matrix

Run `102138` + re-run of tests 2/7 after harness fixes (artifacts in
`.state/results-*/` of the run environment; the interesting ones are quoted
below).

| # | test | result | notes |
|---|---|---|---|
| 1 | `ssh://` build via hook + fail-loud | **PASS** | remote log tail inline in the client error, no re-run needed; rendering duplicated (anomaly 3) |
| 2 | live log streaming mid-build | **FAIL (ssh://)** / PASS (ssh-ng://) | serve 2.9 path delivers **no** build-log lines at all (anomaly 1) |
| 3 | `nix log --store ssh://` after the fact | **PASS** | real log returned by the builder that built it |
| 4 | `--eval-store` + `--store ssh://` | **PASS** | build, no-op re-run, `nix store cat` on remote output all work |
| 5 | cross-client dedup + `active-builds` | **PASS** | one execution (same `token:$$`), 2 subscribers, late joiner gets full replay (60/60 lines) |
| 5b | refcounted cancellation | **FAIL** | first detach decrements correctly; last detach **does not stop the build** (anomaly 2) |
| 6 | `self-scheduled` (elastic, col 9) | **PASS** | maxJobs=1: control peaks at 1 concurrent build, elastic at 2 |
| 7 | `use-ssh-ng-for-remote-builds` | **PASS** | schemeless entry routed over `ssh-ng://` (verified via client copy URI + `nix-daemon` on builder) |
| 8 | OOM-killed build (256Mi pod) | **PASS** | fails as `signal 9 (Killed)`; classification not surfaced (anomaly 4/5) |
| 9 | stock 2.8 builder degradation | **PASS** | build works; `nix log` over ssh fails cleanly: `error: build log of '…' is not available`, rc=1, no hang, no protocol error |

## Anomalies (each with repro)

All repros assume the harness is up (`./local-up.sh`) and are run from the
host; `RUN` is any fresh string (cache-busting salt).

### 1. No build-log delivery over `ssh://` (serve), live or otherwise — **blocker-grade for F-SERVE-DIAG**

Over the hook **and** with `nix-build --store ssh://…` directly, the client
receives **zero** build log lines from a serve-protocol builder — not live,
not at completion. Only the remote process' own stderr chatter (e.g. a
startup warning) arrives, correctly tagged with the build activity
(`bt-slow> warning: …`), which proves the stderr channel and activity
plumbing work. The same drv over `ssh-ng://` streams every line live.
Serve 2.9 *is* negotiated on these connections: `nix log --store ssh://`
(test 3) and the fail-loud tail + `nix log --store` hint (test 1) work on the
same client/builder pair, and the builder persists the log.

```sh
kubectl -n battletest exec client-0 -- bash -c '
  nohup nix build -f /tests/exprs.nix slow --argstr salt r1 \
    --builders "ssh://builder-0.builders x86_64-linux /root/.ssh/id_ed25519 4 1" \
    -L --no-link > /tmp/r.log 2>&1 &
  sleep 15; grep -c bt-slow-line /tmp/r.log'   # -> 0; with ssh-ng:// -> ~14
```

So the builder-side suppression removal and/or the framed-log sidechannel on
serve ≥ 2.9 is not delivering frames into the client's logger on the
`cmdBuildDerivation` path, even though `QueryBuildLog` and the structured
failure diagnostics on the same connection work. The "live build-log
streaming over ssh://" promise of `serve-build-logs` is currently
unobservable end-to-end.

### 2. Refcounted cancellation never kills the build process — **registry/worker disagree**

Killing one of two attached clients correctly drops `subscribers` 2→1 and the
build continues (good). Killing the **last** client removes the registry
entry (`nix store active-builds` → `[]` shortly after) but the build process
on the builder **keeps running to completion** and registers its output:

```sh
kubectl -n battletest exec client-1 -- bash -c '
  nohup bash -c "nix build -f /tests/exprs.nix slower --argstr salt r2 \
    --store ssh-ng://builder-1.builders --eval-store auto -L --no-link \
    > /tmp/c.log 2>&1" & sleep 12
  for f in /proc/[0-9]*/cmdline; do grep -qa "builder-1.builder[s]\|r[2]" "$f" \
    && kill -9 "$(echo "$f" | cut -d/ -f3)"; done; true'
# on builder-1, repeatedly:
kubectl -n battletest exec builder-1 -- bash -c \
  'nix store active-builds --json; ls /nix/store | grep bt-slower'
# -> entry disappears within seconds, but the bt-slower build shell keeps
#    running ~50 more seconds and the output path appears in the store.
```

Observed identically in the matrix (test 5b: `after-kill-both=1` for 30s+).
Either cancellation is intended to abort the `Worker` build and doesn't
(bug), or "cancel" currently only means "forget" — in which case
`active-builds` lies about builder load and an elastic scheduler will
overcommit. The RFC §refcount semantics say cancel-at-zero-refs (no durable
root existed: `rooted:false`).

### 3. Fail-loud block rendered twice, plus a third inconsistent copy

One failing remote build renders, in a single client invocation:
1. `error: build of '…' on 'ssh://builder-0.builders' failed: …` with
   `Last 3 log lines` + `nix log <drv>` hint (no `--store`),
2. the same `Last 3 log lines` block again with the (correct)
   `nix log --store 'ssh://builder-0.builders'` hint,
3. a final `error: Cannot build '…'` block **without** the tail.

Repro: `nix-build /tests/exprs.nix -A fail --argstr salt rX --no-out-link`
on client-0 (full text in `results-102138/t1-fail.log`). Cosmetic, but the
triple rendering makes failures noisy and the first hint
(`nix log <drv>` against the *local* store) is wrong-ish for hook users —
the local store has no log; only the `--store` variant works.

### 4. Failure reason downgraded from "signal 9" to "exit code 1" in the outer error

In the OOM test over the hook, the inner error says
`Reason: builder failed due to signal 9 (Killed).` but the final
client-facing error block for the same failure says
`Reason: builder failed with exit code 1.` (both in
`results-102138/t8.log`). The signal information — exactly what an
OOM-retry layer needs — is lost in the rethrow. Over `ssh-ng://` direct the
single error block correctly says signal 9.

### 5. `ResourceExhausted` / resource hint not observable end-to-end

Per the branch source, SIGKILL'd builds set
`failureClass = ResourceExhausted` and a "possibly OOM-killer" hint on the
`BuildResult`. Surfaces checked: hook+`ssh://` error text, direct
`ssh-ng://` error text (`-L`, `-v`), `nix build --json` (throws before
emitting JSON on failure). None show the class or the hint; the only
observable token is `signal 9 (Killed)`. If schedulers are supposed to key
off `ResourceExhausted`, some CLI/JSON surface needs to carry it (e.g.
`nix build --json --keep-going` emitting per-drv BuildResults).

### 6. `nix build --store ssh://` (new CLI) cannot build at all

`nix build -f … --store ssh://builder-1.builders --eval-store auto` fails
with `error: Unable to build with a primary store that isn't a local store;
either pass a different '--store' or enable remote builds.` while the legacy
`nix-build --eval-store /tmp/eval --store ssh://builder-0.builders …`
succeeds (test 4) and `--store ssh-ng://` works from both CLIs. Given
Phase 4 explicitly enabled `ssh://`+`--eval-store`, the new-CLI path looks
like an unintended gap (different code path deciding "can this store
build?").

### 7. Environment quirks (not protocol findings, but they bit)

- The `docker.nix` image can't run sshd out of the box: root is **locked**
  in `/etc/shadow` (`root:!:…` → `Permission denied (publickey)` with a
  misleading auth error) and there is no `sshd` privsep user. The harness
  entrypoint patches both at pod start.
- `kind load docker-image` fails on multi-platform images under docker's
  containerd snapshotter (`ctr: content digest … not found`); the stock
  image is flattened to a single-platform local tag first.
- This sandbox denies lowering `oom_score_adj`, which kills **every**
  kubelet pod inside kind (`runc … can't get final child's PID from pipe:
  EOF`); `local-up.sh` auto-detects this and uses a node image whose runc
  wrapper strips negative `oomScoreAdj` (see `kind-node/`). It also mounts
  the missing cgroup-v1 hierarchies (`name=systemd`, `cpuset`, `hugetlb`).
- The pod-level 256Mi memory limit *was* enforced (OOM kill observed), even
  on cgroup v1 in a nested sandbox.
- Constant `warning: failed to set up a private mount namespace` noise from
  every nix invocation in unprivileged pods; harmless with `sandbox = false`
  but it pollutes logs and (because it contains the substring `error:`)
  initially broke the harness' error detection. Worth silencing when
  sandbox is disabled.

## Version negotiation observations

- branch client ↔ branch builder: serve 2.9 features all active
  (`QueryBuildLog`, structured diagnostics, fail-loud) — except log
  streaming, see anomaly 1.
- branch client (both features on) ↔ stock 2.8 builder: builds fine over
  `ssh://`, no hangs, no garbage frames; `nix log --store ssh://` fails
  fast and cleanly (`build log of '…' is not available`). Degradation
  contract holds.
- stock builder over the hook also forwards no logs (expected for 2.8 —
  indistinguishable from anomaly 1 from the client's seat, which is itself
  an argument for fixing anomaly 1 before freeze: users can't tell 2.9
  streaming apart from 2.8 silence).

## Phase 3 (Hetzner) status

`hetzner/hetzner-up.sh`, `hetzner-down.sh` + `cluster-config.yaml.tmpl`
(hetzner-k3s, 1× cx32 master + 2× cx32 workers ≈ €0.04/h, no LBs/volumes
created; image is scp'd + `k3s ctr images import`ed onto each node, stock
image pulled+retagged on-node). **Not executed in this session** — no
token available; treat as untested until first real run.
TODO (stretch): ARM worker pool (cax21) for a multi-arch builder.

## Freeze-decision summary

Green: dedup/attach/replay, `active-builds`, elastic builders, ssh-ng
routing opt-in, `nix log` over serve 2.9, fail-loud tail, eval-store split
over serve, OOM kill detection, 2.8 degradation.

Red, in descending severity: (1) serve log streaming missing end-to-end,
(2) cancel-at-zero-refs doesn't stop the worker, (6) new-CLI `--store
ssh://` build gap, (4) signal→exit-code downgrade in the outer error,
(5) `ResourceExhausted` unobservable, (3) duplicated fail-loud rendering.
1 and 2 look like genuine functional gaps the fake-ssh functional tests
didn't catch; they should block the serve-2.9 layout freeze (1) and the
F-INT coordinator sign-off (2) respectively.
