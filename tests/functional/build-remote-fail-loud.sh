#!/usr/bin/env bash

# End-to-end test for the "fail loud" remote-build render: when a build fails
# on a *remote* builder, `build-remote.cc`'s `renderRemoteBuildLogTail` appends the
# tail of the remote build log plus a `nix log --store …` hint to the failure,
# so a remote failure is as informative as a local one. Before this, a remote
# failure was terse and the log was unreachable.
#
# This drives the `ssh-ng://` path (a `RemoteStore`, which is now a `LogStore`
# fetching the log via the `QueryBuildLog` worker op). It uses
# fake-SSH to `localhost` (`SSHMaster`'s `fakeSSH` shortcut runs the remote as
# a local subprocess), so no sshd is required.

source common.sh

TODO_NixOS

# busybox is the (statically linked) builder, copied to the isolated remote.
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

# Avoid the store dir being inside the build sandbox dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* || true

# `--max-jobs 0` routes every build through the build hook (`build-remote.cc`);
# a single `ssh-ng://` builder makes the rendered error deterministic.
builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

# The build must fail.
if out="$(nix build -f fail-loud-remote.nix failingWithLog \
    --arg busybox "$busybox" \
    --no-link --max-jobs 0 \
    --store "$TEST_ROOT/machine0" \
    --builders "$builder" 2>&1)"; then
    echo "expected the remote build to fail, but it succeeded" >&2
    echo "$out" >&2
    exit 1
fi

echo "$out" >&2

# The remote log tail must be surfaced inline, including our marker lines...
echo "$out" | grepQuiet "Last .* log lines:"
echo "$out" | grepQuiet "remote-fail-loud-marker line 3"
# ...and the actionable `nix log` hint must point at the remote store.
echo "$out" | grepQuiet "nix log --store 'ssh-ng://localhost"

# And the same log must actually be fetchable that way.
drvPath="$(nix path-info --store "$TEST_ROOT/machine0" --derivation \
    --arg busybox "$busybox" -f fail-loud-remote.nix failingWithLog)"
nix log --store "ssh-ng://localhost?remote-store=$TEST_ROOT/machine1" "$drvPath" \
    | grepQuiet "remote-fail-loud-marker line 1"
