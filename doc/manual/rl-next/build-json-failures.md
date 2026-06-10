---
synopsis: "`nix build --json` now reports failed builds machine-readably"
prs: []
---

When a build fails, `nix build --json` now still emits its JSON array on
standard output, with one entry per failed derivation carrying the
structured [build result](@docroot@/protocols/json/build-result.md) of the
failure: the failure `status`, the `errorMsg`, the builder's `exitCode`, the
`logTail`, and — when produced by a builder with the experimental structured
diagnostics — the retry classification (`failureClass`, `killedForMemory`)
and `peakMemoryBytes`. Previously standard output was empty whenever the
build failed, so the only failure surface was scraping standard error.

Combined with `--keep-going`, this yields a per-derivation failure report
for an entire build:

```console
$ nix build --json --keep-going ./ci#all | jq '[.[] | select(.success == false)]'
```

Successful entries are unchanged, and the command's exit status and standard
error are the same as without `--json`.
