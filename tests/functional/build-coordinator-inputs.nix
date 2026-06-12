# Regression-test derivations for coordinator-relayed builds of
# derivations with real inputs. `dep`'s output is the builder *script* of
# `withInput`, so `withInput` has a non-empty `inputDrvs` — exactly what
# the relay's `BasicDerivation` wire format used to lose, leaving the
# build sandbox without the input closure (every prior coordinator test
# derivation was input-free, which cannot detect that). Both builders are
# `/bin/sh` — the sandbox shell — so nothing these builds need is visible
# in the sandbox unless the input closure is mounted.
{
  marker,
}:
with import ./config.nix;
rec {
  dep = derivation {
    name = "coordinator-input-dep";
    inherit system;
    builder = "/bin/sh";
    args = [
      "-e"
      (builtins.toFile "make-dep.sh" ''
        {
          echo 'echo ${marker}-via-dep'
          echo 'echo ${marker}-payload > "$out"'
        } > $out
      '')
    ];
  };

  withInput = derivation {
    name = "coordinator-with-input";
    inherit system;
    builder = "/bin/sh";
    # `dep`'s output is this build's script: a true `inputDrvs` input,
    # reachable in the sandbox only through the relayed input closure.
    args = [
      "-e"
      dep
    ];
  };
}
