#!/usr/bin/env bash

# RFC `remote-build-protocol-redesign` Phase 2 (G1 + Gap B): a build offloaded
# to an `ssh://` (legacy serve) builder must stream its log live, upstream.
#
# Before this, the serve protocol suppressed the build log (`verbosity =
# lvlError`) and the client read only the result, so `buildWithHook` had nothing
# to re-emit — an upstream client saw nothing live for an `ssh://` sub-build
# (Gap B). Now the serve `BuildPaths`/`BuildDerivation` commands stream the
# build's log activity as an `STDERR_*` frame sequence (behind the unstable
# serve 2.9 / `serve-build-logs` gate), the client replays it into its logger,
# and the build hook forwards it to the ambient logger — so it tunnels upstream.
#
# This drives the `ssh://` builder via the build hook (`--max-jobs 0`), fake-SSH
# to localhost (no sshd), with a busybox builder so it runs on an isolated
# remote store.

source common.sh

# Offer the provisional serve 2.9 surface on both peers (the fake-SSH localhost
# server is a local subprocess that reads the same test config). Without it the
# peers negotiate 2.8 and nothing streams.
enableFeatures serve-build-logs

TODO_NixOS

# busybox is the (statically linked) builder, copied to the isolated remote.
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

# Avoid the store dir being inside the build sandbox dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* || true

# `--max-jobs 0` routes the build through the build hook; a single `ssh://`
# builder exercises the legacy serve path.
builder="ssh://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

# `-L` makes the (streamed) build log lines visible on stderr. With serve
# streaming + the hook forwarding, the builder's marker line must surface.
out="$(nix build -L -f serve-log-stream.nix withLog \
    --arg busybox "$busybox" \
    --no-link --max-jobs 0 \
    --store "$TEST_ROOT/machine0" \
    --builders "$builder" 2>&1)"

echo "$out" >&2
echo "$out" | grepQuiet "serve-stream-marker-line"
