# A long-running build whose *argv* carries the unique marker (script via
# `sh -c`, not a script file), so the test can observe the live builder
# process with `pgrep -f "$marker"`.
{
  busybox,
  marker,
  iters,
}:
with import ./config.nix;
derivation {
  name = "build-dedup-cancel-last";
  inherit system;
  builder = busybox;
  args = [
    "sh"
    "-e"
    "-c"
    ''
      echo "${marker}"
      # Pure-builtin busy loop: this busybox is shell-only (no `sleep`).
      i=0
      while [ "$i" -lt ${iters} ]; do i=$((i + 1)); done
      echo ok > $out
    ''
  ];
}
