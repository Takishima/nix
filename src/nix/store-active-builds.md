R""(

# Examples

* Show the builds currently in flight on the local build coordinator:

  ```console
  # nix store active-builds
  resolved derivation                                            age   subs   log     rooted
  /nix/store/…-hello-2.12.1.drv                                   12s      2   4.0 KiB  yes
  ```

* As JSON, for scripting:

  ```console
  # nix store active-builds --json
  ```

# Description

Print the builds the **build coordinator** for a store is currently running.

When the [`build-coordinator`](@docroot@/development/experimental-features.md#xp-feature-build-coordinator)
experimental feature is enabled, a `nix-daemon` coalesces concurrent
realisations of the *same resolved derivation* into a single shared build,
hosted by a per-store coordinator process. This command asks that coordinator
for a read-only snapshot of the builds in flight: for each one, the resolved
derivation it is building, how long it has been running, how many clients are
currently attached to its log, how many bytes of log it has produced, and
whether a durable build root keeps it alive even after every client detaches.

The snapshot is the coordinator's authorization-filtered view — on the local
peer-credentialed socket, the builds the calling user may observe.

If no coordinator is running for the store (none has started, or it idle-exited
because nothing is building), there are no active builds and the command prints
an empty list.

)""
