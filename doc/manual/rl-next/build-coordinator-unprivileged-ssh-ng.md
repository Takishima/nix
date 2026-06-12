---
synopsis: "`build-coordinator` now works for unprivileged `ssh-ng://` connections"
---

With the `build-coordinator` experimental feature enabled on a builder, remote
builds over `ssh-ng://` as a non-root [trusted
user](@docroot@/command-ref/conf-file.md#conf-trusted-users) used to fail
immediately: each SSH connection runs `nix-daemon --stdio` as that user, and
that process tried to host the per-store coordinator itself under
`/nix/var/nix` — which is not its directory to create the election lock or
bind the coordinator socket in ("opening the coordinator election lock …:
Permission denied").

A daemon process whose store forwards builds to another daemon (such as the
`--stdio` daemon wrapping the root daemon's Unix-domain socket) now delegates
coordination to the daemon that actually executes the build. Dedup therefore
still works across separate `nix-daemon --stdio` processes serving different
SSH connections — they all funnel into the executing daemon's single,
root-owned coordinator — and root and non-root clients share that same
coordinator. No additional directories, groups or socket permissions are
required on the builder.

Independently, when coordination is genuinely impossible (the coordinator
socket location is not writable by the building process), the build now
degrades to an ordinary uncoordinated build with a warning instead of failing.
