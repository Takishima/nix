with import ./config.nix;

{
  good = mkDerivation {
    name = "json-failures-good";
    builder = builtins.toFile "builder.sh" ''
      echo good > $out
    '';
  };

  bad1 = mkDerivation {
    name = "json-failures-bad1";
    builder = builtins.toFile "builder.sh" ''
      echo "json-failures-bad1-marker line 1"
      echo "json-failures-bad1-marker line 2"
      exit 3
    '';
  };

  bad2 = mkDerivation {
    name = "json-failures-bad2";
    builder = builtins.toFile "builder.sh" ''
      echo "json-failures-bad2-marker"
      exit 4
    '';
  };
}
