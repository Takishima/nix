---
synopsis: "Building on an `ssh://` store now works with `--eval-store`"
---

Building on a legacy-SSH (*serve* protocol) store with a separate eval store,
e.g.

```console
$ nix-build --eval-store /tmp/eval --store ssh://builder
```

used to be rejected with `building on an SSH store is incompatible with
'--eval-store'`. The client now copies the derivation closure (the `.drv`
files and their sources) from the eval store to the build store before
building, exactly as the daemon (`ssh-ng://`) transport already did, and the
remote realises the build from it.

Dependencies already realised in the eval store are copied to the build
store along with the derivations. Note that `nix-store --serve` builds
without substitutes, so any remaining dependencies are built on the remote
side from source.

Legacy-SSH stores also gained a filesystem accessor that streams store
objects as NARs over the connection, so operations that read store contents
— re-running a completed build (which re-reads the derivation), `nix store
cat`, `nix store ls` — now work over `ssh://` instead of failing with
`operation 'getFSAccessor' is not supported`.
