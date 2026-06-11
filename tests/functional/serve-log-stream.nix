{ busybox }:
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
  # A derivation that prints an identifiable line to its build log and then
  # succeeds. Used to check that a build offloaded to an ssh:// (serve) builder
  # streams its log live, upstream.
  withLog = mkDerivation {
    name = "serve-log-stream";
    buildCommand = ''
      echo "serve-stream-marker-line"
      # Create $out as a regular file using only shell builtins (busybox `sh`
      # can't resolve applets like `mkdir` without a standalone-shell PATH).
      echo ok > $out
    '';
  };
}
