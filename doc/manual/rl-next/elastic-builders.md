---
synopsis: "Self-scheduling (elastic) remote builders"
---

A remote build machine can now be marked as *self-scheduling* via a new ninth
field in the [`builders`](@docroot@/command-ref/conf-file.md#conf-builders)
specification (`/etc/nix/machines`):

```
ssh-ng://builder.example.org x86_64-linux - 8 1 - - - true
```

When this field is `true`, the distributed-build hook treats the machine's
maximum-parallel-builds field as a load-balancing **hint** rather than a hard
cap, and never postpones a build because the machine's slots are all busy. This
matches elastic / autoscaling backends (for example
[nixbuild.net](https://nixbuild.net)) that present a single endpoint over a pool
that does its own scheduling, where capping parallelism on the client only gets
in the way.

The flag is strictly opt-in: machines without it keep the existing hard-cap
semantics, so fixed-size builders are never silently overcommitted.
