#!/usr/bin/env bash

# Phase 1 of RFC `remote-build-protocol-redesign` (G8 / Gap C): the
# `print-build-logs = on-failure` mode (`progress-bar.cc`, `main.cc`,
# `globals.hh`). It is the CI-friendly mode: quiet while builds succeed,
# but when a build fails it dumps that build's *full* captured log — more
# than the `log-lines` tail that always accompanies a build error.
#
#   * `print-build-logs = off`   (default): neither live logs nor a full dump
#   * `print-build-logs = on`    (`-L`):    live streaming
#   * `print-build-logs = on-failure`:      quiet on success, full log on failure

source common.sh

TODO_NixOS

clearStore

# A derivation that prints 40 distinctly-numbered log lines and then either
# succeeds or fails. With the default `log-lines = 25`, only the last 25
# lines (16..40) show up in the error tail, so an *early* line such as
# `build-log-line-01` appearing in the output is proof that the *full* log
# was dumped, not just the tail.
mkExpr() {
    # $1: trailing builder command ("false" to fail, "true" to succeed)
    # $2: unique name suffix (so each case is a distinct, uncached derivation)
    cat <<EOF
with import ${config_nix};
mkDerivation {
  name = "plbof-$2";
  buildCommand = ''
    for i in \$(seq -w 1 40); do echo "build-log-line-\$i"; done
    $1
    mkdir -p \$out
  '';
}
EOF
}

build() {
    # $1: print-build-logs value, $2: trailing command, $3: name suffix
    # Echoes combined stdout+stderr; returns the build's exit status.
    nix build -v --no-link --impure \
        --option print-build-logs "$1" \
        --expr "$(mkExpr "$2" "$3")" 2>&1
}

#### 1. on-failure + a failing build: the full log is dumped. ####
if out="$(build on-failure false fail1)"; then
    echo "expected the build to fail, but it succeeded" >&2
    echo "$out" >&2
    exit 1
fi
# The dump header and an *early* line (not in the 25-line tail) must be present.
grepQuiet "full build log for" <<<"$out"
grepQuiet "build-log-line-01" <<<"$out"

#### 2. on-failure + a succeeding build: nothing extra is printed. ####
out="$(build on-failure true ok2)"
grepQuietInverse "full build log for" <<<"$out"
grepQuietInverse "build-log-line-01" <<<"$out"

#### 3. off (default) + a failing build: only the tail, never a full dump. ####
# The error tail still shows the *last* lines, but the early line and the
# on-failure dump header must be absent.
if out="$(build off false fail3)"; then
    echo "expected the build to fail, but it succeeded" >&2
    echo "$out" >&2
    exit 1
fi
grepQuietInverse "full build log for" <<<"$out"
grepQuietInverse "build-log-line-01" <<<"$out"

#### 4. on (live) + a succeeding build: logs stream even on success. ####
out="$(build on true ok4)"
grepQuiet "build-log-line-01" <<<"$out"

#### 5. `-L` is the back-compatible alias for `print-build-logs = on`. ####
out="$(nix build -v -L --no-link --impure --expr "$(mkExpr true ok5)" 2>&1)"
grepQuiet "build-log-line-01" <<<"$out"
