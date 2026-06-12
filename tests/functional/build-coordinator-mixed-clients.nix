{
  busybox,
  name,
  marker,
  iters,
}:

with import ./config.nix;

# A classic (unresolved) input-addressed derivation WITH inputDrvs: `dep`
# rides along as a real derivation input, so both relayers must apply the
# inputSrcs hijack and arrive at byte-identical request bodies — the
# registry key. An input-free derivation would let the two paths diverge
# invisibly.
let
  dep = derivation {
    name = "${name}-dep";
    inherit system;
    builder = busybox;
    args = [
      "sh"
      "-e"
      "-c"
      "echo dep-payload > $out"
    ];
  };
in
derivation {
  inherit name system dep;
  builder = busybox;
  args = [
    "sh"
    "-e"
    "-c"
    ''
      echo "${marker}"
      read -r buildtoken < /proc/sys/kernel/random/uuid
      echo "BUILDTOKEN:$buildtoken"
      # Shell-only busybox: a builtin loop holds the build in-flight so
      # the second protocol's client can attach.
      i=0
      while [ "$i" -lt ${toString iters} ]; do i=$((i + 1)); done
      echo ok > $out
    ''
  ];
}
