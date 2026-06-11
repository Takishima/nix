#!/usr/bin/env bash

# Regression test: `nix log` must work over `ssh-ng://`. Before this, `SSHStore::getBuildLogExact`
# threw `unsupported`; it is now implemented in `RemoteStore` via the
# `QueryBuildLog` worker-protocol op, so a `ssh-ng://` client can fetch a build
# log that the remote daemon persisted.

source common.sh

# Drives a real remote daemon over `ssh-ng://localhost` (fake-SSH: the daemon is
# run as a local subprocess, so no sshd is needed), like the other ssh-ng tests.
TODO_NixOS

clearStore

# Build locally so the daemon's store persists the build log on disk.
# `dependencies.nix`'s top derivation logs exactly `FOO` (see logging.sh).
outPath=$(nix-build dependencies.nix --no-out-link)
[ "$(nix-store -l "$outPath")" = FOO ]

# Fetch the same build log over `ssh-ng://localhost`. The fake-SSH daemon
# inherits this test's store/log dirs, so the log it serves is the one built
# above; the point is that the *client* is a plain `RemoteStore` (not a
# `LocalFSStore`) and must go through the new `QueryBuildLog` worker op.
#
# Before this was fixed it failed with
#   "operation 'getBuildLogExact' is not supported by store 'ssh-ng://localhost'".
[ "$(nix log --store ssh-ng://localhost "$outPath")" = FOO ]
