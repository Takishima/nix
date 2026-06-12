{
  busybox,
  name,
  marker,
  iters,
}:

with import ./config.nix;

# One slow, observable build per GC round (`name` differs): marker, a
# kernel-uuid token, then a builtin busy loop (shell-only busybox has no
# `sleep`) so garbage collection demonstrably overlaps the build.
derivation {
  inherit name system;
  builder = busybox;
  args = [
    "sh"
    "-e"
    "-c"
    ''
      echo "${marker}"
      read -r buildtoken < /proc/sys/kernel/random/uuid
      echo "BUILDTOKEN:$buildtoken"
      i=0
      while [ "$i" -lt ${toString iters} ]; do i=$((i + 1)); done
      echo "${name}-done"
      echo ok > $out
    ''
  ];
}
