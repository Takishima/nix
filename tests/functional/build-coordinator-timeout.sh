#!/usr/bin/env bash

# Coordination must not change build-timeout semantics: a build-timeout
# configured on the *builder* is enforced on a coordinated build exactly as on
# an ordinary remote build. The coordinator's build child runs the real builder
# with respectTimeouts=true (derivation-building-goal.cc), so the timeout fires
# in the child and its TimedOut result propagates back through the relay to the
# client.
#
# This pins the outcome of the investigation into the field report about
# per-invocation timeouts: a submitter's own `--timeout` is NOT propagated to a
# remote builder (the daemon protocol's SetOptions handshake is disabled by the
# `disable-set-options` feature — this is upstream behavior, identical for
# uncoordinated remote builds), so the reachable, meaningful timeout is the one
# configured on the builder, and it must survive the coordinator/relay path
# rather than being swallowed by it. There is no coordinator-side timeout to
# special-case; this test guards that the ordinary one keeps working through
# coordination.
#
# Topology is build-dedup-coordinator.sh's, single client: the derivation is
# offloaded to one ssh-ng builder whose daemon hosts the coordinator and whose
# config carries `timeout`.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# The builder's configured build timeout, in seconds. Applied via the shared
# test config, which the ssh-ng builder daemon (and thus the coordinator and its
# build child) reads.
echo "timeout = 3" >> "${test_nix_conf?}"

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/timeout-"* || true

marker="build-coord-timeout-marker-$$"
# ~444K iters/s in busybox ash here, so ~13M ≈ 30s: far longer than the 3s
# builder timeout, so a passing run means the timeout fired (not that the build
# simply finished). The busy loop is silent, so this is `timeout`, not
# `max-silent-time`, doing the killing.
iters=13000000

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/timeout-coord.sock"

builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

out="$TEST_ROOT/timeout.out"

set +e
nix build -L -f build-dedup-coordinator.nix slow \
    --arg busybox "$busybox" \
    --argstr marker "$marker" --argstr iters "$iters" \
    --no-link --max-jobs 0 \
    --store "$TEST_ROOT/machine0" \
    --builders "$builder" > "$out" 2>&1
rc=$?
set -e

# The coordinated build is killed by the builder's timeout and the failure
# reaches the client.
[ "$rc" -ne 0 ] || { echo "coordinated build was not timed out (timeout swallowed by the coordinator path)" >&2; cat "$out" >&2; exit 1; }
grepQuiet "timed out" "$out" \
    || { echo "coordinated build failed, but not with a timeout" >&2; cat "$out" >&2; exit 1; }

# It really went through the coordinator (a degraded/uncoordinated build would
# have warned) — so the timeout fired on the coordinated path, not around it.
grepQuietInverse "without build dedup" "$out"

# The build actually started under coordination before timing out (its marker
# streamed through the relay).
grepQuiet "$marker" "$out" \
    || { echo "the build never streamed its marker before timing out" >&2; cat "$out" >&2; exit 1; }

echo "timeout OK: a builder-configured timeout fired on a coordinated build and the failure reached the client" >&2
