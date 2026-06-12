{ name }:

with import ./config.nix;

# A fresh, never-cached derivation per scenario (`name` differs), so each
# build actually reaches the coordinator relay.
mkDerivation {
  inherit name;
  buildCommand = "echo $name > $out";
}
