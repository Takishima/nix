#!/usr/bin/env bash

# Regression test for "Gap A" (RFC `remote-build-protocol-redesign`, Phase 0)
# over the legacy `nix-store --serve` protocol (`ssh://`, used by
# `hydra-queue-runner`): `nix log` must work without out-of-band log capture.
#
# This exercises both halves of the serve fix, behind the provisional serve 2.9
# offered by the `serve-build-logs` experimental feature:
#   * the builder persists the build log (keepLog made conditional), and
#   * the client fetches it back via the new `QueryBuildLog` serve command
#     (LegacySSHStore is now a LogStore).

source common.sh

# Offer the provisional serve 2.9 surface on both peers (the fake-SSH localhost
# server is a local subprocess that reads the same test config).
enableFeatures serve-build-logs

TODO_NixOS

clearStore

# Build *through* the serve store so the remote builder persists the log.
# `dependencies.nix`'s top derivation logs exactly `FOO` (see logging.sh).
outPath=$(nix build --no-link --print-out-paths --store ssh://localhost -f dependencies.nix)

# Fetch the build log back over the serve protocol (QueryBuildLog). Before this,
# `nix log` over `ssh://` skipped the store entirely ("does not support
# retrieving build logs").
[ "$(nix log --store ssh://localhost "$outPath")" = FOO ]
