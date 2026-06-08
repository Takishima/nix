#!/usr/bin/env bash

# Regression test for `nix log` over the legacy `nix-store --serve` protocol (`ssh://`, used by
# `hydra-queue-runner`): `nix log` must work without out-of-band log capture.
#
# Exercises the new serve `QueryBuildLog` command, reachable only at the
# provisional serve 2.9 offered by the `serve-build-logs` experimental feature.
# `LegacySSHStore` is now a `LogStore`, so `nix log --store ssh://...` fetches
# the log over the serve protocol instead of skipping the store.

source common.sh

# Offer the provisional serve 2.9 surface on both peers (the fake-SSH localhost
# server is a local subprocess that reads the same test config).
enableFeatures serve-build-logs

TODO_NixOS

clearStore

# Build locally so the log is persisted; the fake-SSH `nix-store --serve` shares
# this store, so it can serve that log back. `dependencies.nix`'s top derivation
# logs exactly `FOO` (see logging.sh).
outPath=$(nix-build dependencies.nix --no-out-link)
[ "$(nix-store -l "$outPath")" = FOO ]

# Fetch the build log over `ssh://localhost` (legacy serve, QueryBuildLog).
# Before this, `nix log` over `ssh://` skipped the store entirely ("does not
# support retrieving build logs").
[ "$(nix log --store ssh://localhost "$outPath")" = FOO ]

# Without the feature, the serve store offers only 2.8 and cannot serve logs.
# `nix log` must degrade gracefully: `getBuildLogExact` returns "no log here"
# (rather than erroring), so the command fails with the normal "not available"
# rather than an "unsupported operation" crash.
sed -i 's/ serve-build-logs//' "${test_nix_conf?}"
expect 1 nix log --store ssh://localhost "$outPath" 2>/dev/null
