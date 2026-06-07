{
  busybox,
  marker,
  iters,
}:
with import ./config.nix;
let
  mkDerivation =
    args:
    derivation (
      {
        inherit system;
        builder = busybox;
        args = [
          "sh"
          "-e"
          args.builder or (builtins.toFile "builder-${args.name}.sh" ''
            if [ -e "$NIX_ATTRS_SH_FILE" ]; then source $NIX_ATTRS_SH_FILE; fi;
            eval "$buildCommand"
          '')
        ];
      }
      // removeAttrs args [
        "builder"
        "meta"
      ]
    )
    // {
      meta = args.meta or { };
    };
in
{
  # A derivation whose build (1) prints an identifiable marker, (2) prints a
  # token unique to *this build process* (the builder shell's PID — distinct for
  # every real build invocation), then (3) stays in-flight for `seconds` so a
  # second, concurrent request for the *same* resolved derivation can attach to
  # it (RFC remote-build-protocol-redesign Phase 3, G3). The build runs in an
  # isolated mount namespace, so it communicates *only* through its streamed log
  # — which is exactly the fan-out/replay path under test. The test proves dedup
  # by asserting both clients observe the *same* token (one shared build), the
  # late joiner having received it via replay.
  slow = mkDerivation {
    name = "build-dedup-coordinator";
    buildCommand = ''
      echo "${marker}"
      echo "BUILDTOKEN:$$"
      # Hold the build in-flight long enough for the second request to attach.
      # This sandbox-shell busybox has no `sleep` applet (it is shell-only), so
      # we burn wall-clock with a pure-builtin loop. The test does not rely on
      # the exact duration: it proves the attach by polling for the late
      # joiner's *replayed* marker, so the loop only has to outlast that attach.
      i=0
      while [ "$i" -lt ${toString iters} ]; do i=$((i + 1)); done
      echo ok > $out
    '';
  };
}
