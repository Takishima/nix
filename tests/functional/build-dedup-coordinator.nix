{
  busybox,
  count,
  started,
  fifo,
  marker,
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
  # A derivation whose build (1) prints an identifiable marker line to its log,
  # (2) records that a *real* build ran by appending to `count`, then (3) blocks
  # on a FIFO until the test releases it. The block keeps exactly one build
  # in-flight long enough for a second, concurrent request for the *same*
  # resolved derivation to attach to it (RFC remote-build-protocol-redesign
  # Phase 3, G3). Only shell builtins are used (echo, read, redirection) so it
  # runs under busybox `sh` without needing applets on PATH.
  slow = mkDerivation {
    name = "build-dedup-coordinator";
    buildCommand = ''
      echo "${marker}"
      echo x >> "${count}"
      : > "${started}"
      # Block until the test writes to the FIFO (pure `read` builtin — no sleep).
      read _ < "${fifo}"
      echo ok > $out
    '';
  };
}
