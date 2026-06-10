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

# ...but exactly ONCE: the remote failure message already embeds the tail
# rendered by the builder-side goal, so `renderRemoteBuildLogTail` must not
# repeat it (battletest regression: the tail block was rendered twice, with
# two different `nix log` hints).
[ "$(echo "$out" | grep -c "remote-fail-loud-marker line 3")" = 1 ] || {
    echo "the remote log tail was rendered more than once" >&2
    exit 1
}

# And the same log must actually be fetchable that way.
drvPath="$(nix path-info --store "$TEST_ROOT/machine0" --derivation \
    --arg busybox "$busybox" -f fail-loud-remote.nix failingWithLog)"
nix log --store "ssh-ng://localhost?remote-store=$TEST_ROOT/machine1" "$drvPath" \
    | grepQuiet "remote-fail-loud-marker line 1"

# A builder killed by a signal (OOM-killer style) must be reported as such:
# the goal must not overwrite the builder's death-by-signal with the build
# *hook's* own exit code (battletest regression: the final error said
# "builder failed with exit code 1" after a SIGKILLed remote build).
if command -v pgrep > /dev/null; then
    killedOut="$TEST_ROOT/killed.out"
    startKilled() {
        exec nix build -f fail-loud-remote.nix killedBuilder \
            --arg busybox "$busybox" \
            --no-link --max-jobs 0 \
            --store "$TEST_ROOT/machine0" \
            --builders "$builder" > "$killedOut" 2>&1
    }
    startKilled &
    clientPid=$!

    i=0
    while ! pgrep -f "remote-fail-killed-marke[r]" > /dev/null; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt 300 ] || { echo "killed-builder never started" >&2; cat "$killedOut" >&2; kill "$clientPid"; exit 1; }
    done
    pkill -9 -f "remote-fail-killed-marke[r]"

    if wait "$clientPid"; then
        echo "expected the killed remote build to fail, but it succeeded" >&2
        cat "$killedOut" >&2
        exit 1
    fi

    cat "$killedOut" >&2
    grepQuiet "due to signal 9" "$killedOut"
    grepQuietInverse "builder failed with exit code" "$killedOut"
fi
