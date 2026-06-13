{
  busybox,
  name,
  marker,
  iters,
}:

with import ./config.nix;

# A build whose log goes silent for a long stretch in the MIDDLE: print a
# marker and a kernel-uuid token, then burn wall-clock in a pure-builtin
# loop emitting NOTHING (this sandbox-shell busybox has no `sleep`; a busy
# loop produces no stdout/stderr, which is exactly the silent-but-running
# state real builds reach during LTO links, test suites, tarball unpacks),
# then print DONE. The silence window is set by `iters`. The point is that
# no coordinator/relay bound may kill a running-but-quiet build.
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
      echo "${name}-DONE"
      echo ok > $out
    ''
  ];
}
