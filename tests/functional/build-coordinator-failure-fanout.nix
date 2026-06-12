{
  busybox,
  name,
  marker,
  iters,
}:

with import ./config.nix;

{
  # A build that emits a marker, a kernel-uuid token and a last-words line,
  # stays in-flight long enough for staggered attachers, then fails with a
  # recognizable exit code.
  failing = derivation {
    inherit name system;
    builder = busybox;
    args = [
      "sh"
      "-c"
      ''
        echo "${marker}"
        read -r buildtoken < /proc/sys/kernel/random/uuid
        echo "BUILDTOKEN:$buildtoken"
        i=0
        while [ "$i" -lt ${toString iters} ]; do i=$((i + 1)); done
        echo "FAILLINE:last words before death"
        exit 7
      ''
    ];
  };

  # A fresh, unrelated derivation: must build cleanly after the failure
  # storm (no poisoned daemon or coordinator).
  ok = derivation {
    name = "${name}-ok";
    inherit system;
    builder = busybox;
    args = [
      "sh"
      "-e"
      "-c"
      "echo ok > $out"
    ];
  };
}
