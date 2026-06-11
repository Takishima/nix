#!/usr/bin/env bash

# Building on an `ssh://` (legacy serve) store with a separate `--eval-store`
# used to be rejected up front ("building on an SSH store is incompatible with
# '--eval-store'"). The client now copies the derivation closure (.drvs and
# sources) from the eval store to the build store first — the same
# choreography the daemon (`ssh-ng://`) path uses — and the remote realises
# the inputs itself. This exercises the split end-to-end over fake-SSH to
# localhost (no sshd).

source common.sh

TODO_NixOS

# `--eval-store` only makes sense with a local orchestrating store.
needLocalStore "'--eval-store' doesn't achieve much with the daemon"

eval_store="$TEST_ROOT/eval-store"

clearStore
rm -rf "$eval_store"

outPath=$(nix-build dependencies.nix --no-out-link \
    --eval-store "$eval_store" --store ssh://localhost)

[[ -e $outPath/foobar ]]

# The split is real: the derivation was evaluated into the eval store (and
# copied to the build store only for building)...
ls "$eval_store"/nix/store/*-dependencies-top.drv >/dev/null

# ...and the outputs land on the build store, not in the eval store.
(! ls "$eval_store"/nix/store/*dependencies-top/foobar)

# A second invocation is a no-op (the outputs are already valid on the
# build store). This also exercises reading the now-valid derivation back
# from the build store (`queryMissing`), which streams it as a NAR over
# the serve connection.
outPath2=$(nix-build dependencies.nix --no-out-link \
    --eval-store "$eval_store" --store ssh://localhost)
[[ $outPath = "$outPath2" ]]

# Store objects on the build store are readable through the same NAR
# streaming.
[[ "$(nix store cat --store ssh://localhost "$outPath/foobar")" = "$(cat "$outPath/foobar")" ]]

# Inputs already realised in the eval store are copied to the builder rather
# than rebuilt there (the serve side builds without substitutes). Use a
# post-build hook on the builder as the witness of what actually got built.
clearStore
rm -rf "$eval_store"

echo "post-build-hook = $PWD/build-hook-list-paths.sh" >> "${test_nix_conf?}"
export HOOK_DEST=$TEST_ROOT/hook-log

# Get one dependency's output into the eval store: build it locally, copy it
# over, then clear the local (= build) store again.
topDrv=$(nix-instantiate dependencies.nix)
input2Drv=$(nix-store -q --references "$topDrv" | grep dependencies-input-2)
input2Out=$(nix-store --realise "$input2Drv")
nix copy --to "$eval_store" --no-check-sigs "$input2Out"
clearStore

rm -f "$HOOK_DEST"
outPath=$(nix-build dependencies.nix --no-out-link \
    --eval-store "$eval_store" --store ssh://localhost)
[[ -e $outPath/foobar ]]

# The builder built the rest of the closure...
grepQuiet dependencies-top "$HOOK_DEST"
# ...but not the prebuilt input: its output was copied from the eval store.
grepQuietInverse dependencies-input-2 "$HOOK_DEST"

# The new CLI takes the same split: `nix build` resolves builds through
# `buildPathsWithResults`, which used to fall into the generic in-process
# Worker and be rejected with "Unable to build with a primary store that
# isn't a local store".
clearStore
rm -rf "$eval_store"

outPath=$(nix build -f dependencies.nix --no-link --print-out-paths \
    --eval-store "$eval_store" --store ssh://localhost)
[[ -e $outPath/foobar ]]

# Re-running is a no-op and reports the same outputs.
outPath2=$(nix build -f dependencies.nix --no-link --print-out-paths \
    --eval-store "$eval_store" --store ssh://localhost)
[[ $outPath = "$outPath2" ]]
