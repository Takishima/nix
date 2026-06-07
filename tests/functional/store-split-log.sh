#!/usr/bin/env bash

# RFC `remote-build-protocol-redesign` Phase 4 (G4): logs behave identically
# when the build store differs from the eval store. Logs are keyed on the
# derivation, so live streaming and `nix log` (fetch, Gap A) carry through the
# split with no special wiring.
#
# This exercises the `ssh-ng://` build store with a separate local `--eval-store`
# (the build runs on the build store, which is a *different* store from where
# the derivation was evaluated), using fake-SSH to localhost (no sshd).
#
# Note: building on an `ssh://` (legacy serve) store with a separate eval store
# is a separate, deferred item (RFC §4.6's `realiseRemote(...)`): the serve
# build model can't realise a copied drv closure on its own. `ssh-ng://` is the
# split transport that works today, which is what G4's log parity rides on.

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

# `nix log` over the split fetches the log from the *build* store (Gap A),
# keyed on the derivation, regardless of the eval store — identical to the
# non-split case.
[ "$(nix log --store ssh-ng://localhost --eval-store "$eval_store" "$outPath")" = FOO ]

# And after the fact, querying the build store directly returns the same log.
[ "$(nix log --store ssh-ng://localhost "$outPath")" = FOO ]
