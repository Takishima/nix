---
synopsis: "Opt into `ssh-ng://` for schemeless remote builders"
---

A remote build machine given without a URL scheme — e.g. `mac` or `nix@mac` in
[`builders`](@docroot@/command-ref/conf-file.md#conf-builders) /
`/etc/nix/machines`, rather than `ssh://mac` — has always defaulted to the
legacy *serve* protocol (`ssh://`), which does not stream the remote builder's
log back to the client.

The new [`use-ssh-ng-for-remote-builds`](@docroot@/command-ref/conf-file.md#conf-use-ssh-ng-for-remote-builds)
setting makes such schemeless builders use the `ssh-ng://` transport instead,
which streams build logs live. This is the direction remote building is
converging on; the setting lets an operator move a whole fleet over without
rewriting every machine entry.

It defaults to `false`, so existing configurations keep their current transport,
and builders written with an explicit scheme are never affected.
