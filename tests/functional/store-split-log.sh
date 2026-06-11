#!/usr/bin/env bash

# Logs behave identically
# when the build store differs from the eval store. Logs are keyed on the
# derivation, so live streaming and `nix log` (fetch) carry through the
# split with no special wiring.
#
# This exercises the `ssh-ng://` build store with a separate local `--eval-store`
# (the build runs on the build store, which is a *different* store from where
# the derivation was evaluated), using fake-SSH to localhost (no sshd).
#
# Note: the same split over the `ssh://` (legacy serve) store is exercised by
# store-split-serve.sh.

source common.sh

TODO_NixOS

# `--eval-store` only makes sense with a local orchestrating store.
needLocalStore "'--eval-store' doesn't achieve much with the daemon"

eval_store="$TEST_ROOT/eval-store"

clearStore
rm -rf "$eval_store"

# Build with eval store != build store. `dependencies.nix`'s top derivation
# logs exactly `FOO` (see logging.sh); the build runs on the build store.
outPath=$(nix build -f dependencies.nix --no-link --print-out-paths \
    --eval-store "$eval_store" --store ssh-ng://localhost)

# The split is real: the .drv lives in the eval store (it was copied to the
# build store only for building).
ls "$eval_store"/nix/store/*-dependencies-top.drv >/dev/null

# `nix log` over the split fetches the log from the *build* store,
# keyed on the derivation, regardless of the eval store — identical to the
# non-split case.
[ "$(nix log --store ssh-ng://localhost --eval-store "$eval_store" "$outPath")" = FOO ]

# And after the fact, querying the build store directly returns the same log.
[ "$(nix log --store ssh-ng://localhost "$outPath")" = FOO ]
