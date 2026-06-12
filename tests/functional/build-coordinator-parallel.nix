# Two *different* slow derivations whose argv carries a per-derivation marker
# (script via `sh -c`, not a script file), so the test can observe the live
# builder processes with `pgrep -f`.
{
  marker,
  iters,
}:
with import ./config.nix;
let
  slow =
    tag:
    derivation {
      name = "coordinator-parallel-${tag}";
      inherit system;
      builder = shell;
      args = [
        "-e"
        "-c"
        ''
          echo "${marker}-${tag}"
          # Pure-builtin busy loop (no PATH in here): long enough for the test
          # to observe both builders alive at once, short enough to terminate
          # the test promptly either way.
          i=0
          while [ "$i" -lt ${iters} ]; do i=$((i + 1)); done
          echo ok > $out
        ''
      ];
    };
in
{
  a = slow "A";
  b = slow "B";
}
