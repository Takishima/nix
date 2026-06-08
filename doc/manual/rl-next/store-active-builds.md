---
synopsis: "New `nix store active-builds` command"
---

When the `build-coordinator` experimental feature is enabled, a `nix-daemon`
coalesces concurrent realisations of the same resolved derivation into a single
shared build hosted by a per-store coordinator. The new `nix store
active-builds` command asks that coordinator for a read-only snapshot of the
builds it is currently running:

```console
# nix store active-builds
age      subs  log        rooted  resolved derivation
12s      2     4.0 KiB    yes     /nix/store/…-hello-2.12.1.drv
```

For each in-flight build it reports the resolved derivation being built, how
long it has been running, how many clients are attached to its log, how many
bytes of log it has produced, and whether a durable build root keeps it alive
after every client detaches. Pass `--json` for machine-readable output.

The snapshot is the coordinator's authorization-filtered view — on the local
peer-credentialed socket, the builds the calling user may observe. If no
coordinator is running, the list is empty.
