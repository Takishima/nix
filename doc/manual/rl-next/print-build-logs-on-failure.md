---
synopsis: "New `print-build-logs = on-failure` mode"
---

The `print-build-logs` setting now accepts three values instead of being a
plain on/off toggle:

- `off` (default): only a progress indicator is shown.
- `on`: build logs are streamed live, exactly like passing `--print-build-logs`
  (`-L`).
- `on-failure`: build logs stay hidden while builds succeed, but when a build
  fails its full log is printed.

`on-failure` is convenient for CI: it stays quiet on success and dumps the
complete log of whatever broke, so you no longer have to decide to pass `-L`
*before* knowing a build will fail.

The `-L` / `--print-build-logs` flag is unchanged and maps to
`print-build-logs = on`. The new mode is selected via the setting, e.g.
`--option print-build-logs on-failure` or `print-build-logs = on-failure` in
`nix.conf`.
