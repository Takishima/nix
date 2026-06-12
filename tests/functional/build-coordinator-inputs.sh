#!/usr/bin/env bash

# A coordinator-relayed build must carry the derivation's *full input
# closure*. The relay ships a `BasicDerivation` — `inputDrvs` does not
# survive the wire — and a classic input-addressed derivation's sandbox
# is populated from `inputDrvs`: before the fix, any derivation with a
# real input failed in the coordinator's build child with
#
#     error: executing '<builder>': No such file or directory
#
# while every existing coordinator test (input-free derivations) passed.
# The builds here are *sandboxed* precisely so that a missing input
# closure is a hard error instead of silently reading the host store.
#
# Leg 1: a local client with a LocalStore — the goal-level relay
#        (the deployed shape: root building on the machine itself).
# Leg 2: the unprivileged ssh-ng path — stdio daemon forwarding to a
#        shared daemon whose connection child relays.

source common.sh

TODO_NixOS

needLocalStore "the builds must run (sandboxed) on this side"

requireSandboxSupport
requiresUnprivilegedUserNamespaces

enableFeatures build-coordinator

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT"/coordinput-store "$TEST_ROOT"/machine* 2>/dev/null || true
rm -rf "$TEST_ROOT"/coordinput-store "$TEST_ROOT"/machine* || true

marker="coordinput-$$"

# ---- Leg 1: goal-level relay (local LocalStore client) ----

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coordinput.sock"

out1="$TEST_ROOT/coordinput-local.out"
outPath1=$(nix build -L -f build-coordinator-inputs.nix withInput \
    --argstr marker "$marker" \
    --option sandbox true --option sandbox-fallback false \
    --no-link --print-out-paths \
    --store "$TEST_ROOT/coordinput-store" 2> "$out1") \
    || { echo "local coordinated build with inputs failed" >&2; cat "$out1" >&2; exit 1; }

# The dep's script — an `inputDrvs` input — really ran (its log line
# streamed through the coordinator, its payload is the output)...
grepQuiet -- "$marker-via-dep" "$out1"
nix store cat --store "$TEST_ROOT/coordinput-store" "$outPath1" | grepQuiet -- "$marker-payload"
# ...and via the coordinator, not an uncoordinated fallback.
grepQuietInverse "without build dedup" "$out1"

# ---- Leg 2: unprivileged ssh-ng (stdio daemon → shared daemon relay) ----

daemonSock="$TEST_ROOT/coordinput-daemon.sock"

# The shared daemon (stands in for the root daemon) builds sandboxed and
# is the only process given a usable coordinator socket.
NIX_DAEMON_SOCKET_PATH="$daemonSock" \
NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coordinput-fwd.sock" \
NIX_CONFIG=$'sandbox = true\nsandbox-fallback = false' \
    nix daemon --store "$TEST_ROOT/machine1" &
daemonPid=$!
trap 'kill "$daemonPid" 2>/dev/null || true' EXIT

for ((i = 0; i < 100; i++)); do
    if [[ -S "$daemonSock" ]]; then break; fi
    kill -0 "$daemonPid" || { echo "shared daemon died" >&2; exit 1; }
    sleep 0.1
done
[[ -S "$daemonSock" ]] || { echo "shared daemon never bound its socket" >&2; exit 1; }

# The stdio daemons (fake-SSH) must delegate, not coordinate themselves.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/coordinput.sock"

# See build-dedup-coordinator-forwarded.sh for the `root=` rationale.
builder="ssh-ng://localhost?remote-store=unix://$daemonSock%3Froot=$TEST_ROOT/machine1 - - 1 1"

out2="$TEST_ROOT/coordinput-sshng.out"
outPath2=$(nix build -L -f build-coordinator-inputs.nix withInput \
    --argstr marker "$marker-fwd" \
    --no-link --print-out-paths --max-jobs 0 \
    --store "$TEST_ROOT/machine0c" \
    --builders "$builder" 2> "$out2") \
    || { echo "forwarded coordinated build with inputs failed" >&2; cat "$out2" >&2; exit 1; }

grepQuiet -- "$marker-fwd-via-dep" "$out2"
nix store cat --store "$TEST_ROOT/machine0c" "$outPath2" | grepQuiet -- "$marker-fwd-payload"
grepQuietInverse "without build dedup" "$out2"

echo "coordinated builds with real inputs OK (local + forwarded ssh-ng)" >&2
