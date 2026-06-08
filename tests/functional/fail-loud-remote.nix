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
  # A derivation that writes identifiable lines to its build log and then
  # fails. Used to check that a *remote* build failure surfaces the remote
  # log tail. Uses busybox
  # (statically linked, self-contained) so it builds on an isolated remote.
  failingWithLog = mkDerivation {
    name = "fail-loud-remote";
    buildCommand = ''
      echo "remote-fail-loud-marker line 1"
      echo "remote-fail-loud-marker line 2"
      echo "remote-fail-loud-marker line 3"
      exit 1
    '';
  };
}
