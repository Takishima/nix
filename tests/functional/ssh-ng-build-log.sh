#!/usr/bin/env bash

# Regression test for "Gap A" (RFC `remote-build-protocol-redesign`, Phase 0):
# `nix log` must work over `ssh-ng://`. Before this, `SSHStore::getBuildLogExact`
# threw `unsupported`; it is now implemented in `RemoteStore` via the
# `QueryBuildLog` worker-protocol op, so a `ssh-ng://` client can fetch a build
# log that the remote daemon persisted.

source common.sh

# Like the other `ssh-ng://localhost` tests, this drives a real remote daemon.
TODO_NixOS

clearStore

mkdir -p "$TEST_ROOT/stores"
remoteRoot="$TEST_ROOT/stores/ssh-ng-build-log"
chmod -R u+w "$remoteRoot" 2>/dev/null || true
rm -rf "$remoteRoot"

storeQueryParam="store=${NIX_STORE_DIR}"
# An `ssh-ng://localhost` store whose remote daemon uses an isolated chroot store
# (so the build log is persisted there and must be fetched back over the
# protocol, not read from the local disk).
remoteStore="ssh-ng://localhost?${storeQueryParam}&remote-store=${remoteRoot}%3f${storeQueryParam}%26real=${remoteRoot}${NIX_STORE_DIR}"

# Build into the remote store; its daemon writes the build log to its own log dir.
# `dependencies.nix`'s top derivation logs exactly `FOO` (see logging.sh).
outPath=$(nix build --no-link --print-out-paths --no-check-sigs --store "$remoteStore" -f dependencies.nix)

# Fetch the build log back over ssh-ng. This is the path that previously failed
# with "operation 'getBuildLogExact' is not supported by store 'ssh-ng://...'".
[ "$(nix log --store "$remoteStore" "$outPath")" = FOO ]
