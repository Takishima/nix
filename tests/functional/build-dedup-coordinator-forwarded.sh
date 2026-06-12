#!/usr/bin/env bash

# The unprivileged `ssh-ng://` path: each SSH connection runs its own
# `nix-daemon --stdio` *as the SSH user*, whose store is the real daemon's
# `UDSRemoteStore` — a process that can NOT host the coordinator (its
# socket would live in the daemon's state dir, which is not the SSH
# user's to bind in). The stdio daemon must therefore *delegate*: forward
# `BuildDerivation` to the daemon at the other end of the socket, whose
# own connection handler relays to the one coordinator — so dedup still
# works across separate stdio daemons serving different SSH connections.
#
# Topology (all unprivileged):
#
#   nix build (machine0a) ─ssh-ng→ nix-daemon --stdio ─unix://→ ┐
#                                   (forwards, no election)     ├─ nix-daemon (machine1) ─→ coordinator
#   nix build (machine0b) ─ssh-ng→ nix-daemon --stdio ─unix://→ ┘   (its children relay)
#
# To prove the stdio daemons really delegate (rather than electing — or
# degrading — locally), their environment points the coordinator socket at
# an unusable location; only the shared daemon gets a usable one. Dedup is
# asserted exactly like build-dedup-coordinator.sh: both clients must
# observe the SAME per-build-process token, the late joiner via replay.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/fwd-"* || true

# The shared daemon authenticates the unix-socket peer: the stdio daemons
# connect as us, and building an input-addressed derivation (and copying
# unsigned inputs) needs a trusted user — exactly the deployed
# `trusted-users = [ nixremote ]` shape. The test config already has
# `trusted-users = $(whoami)`.

daemonSock="$TEST_ROOT/fwd-daemon.sock"

# The real daemon (stands in for the root daemon): serves the machine1
# store, and is the only process given a usable coordinator socket.
NIX_DAEMON_SOCKET_PATH="$daemonSock" \
NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/fwd-coord.sock" \
    nix daemon --store "$TEST_ROOT/machine1" &
daemonPid=$!
trap 'kill "$daemonPid" 2>/dev/null || true' EXIT

for ((i = 0; i < 100; i++)); do
    if [[ -S "$daemonSock" ]]; then break; fi
    kill -0 "$daemonPid" || { echo "shared daemon died" >&2; exit 1; }
    sleep 0.1
done
[[ -S "$daemonSock" ]] || { echo "shared daemon never bound its socket" >&2; exit 1; }

# Everything else (the clients, and through fake-SSH the stdio daemons)
# sees an unusable coordinator location: were a stdio daemon to elect (or
# degrade) instead of delegating, it would warn "without build dedup" —
# asserted absent below.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/coord.sock"

marker="build-dedup-fwd-marker-$$"
# See build-dedup-coordinator.sh: a pure-builtin busy loop keeps the build
# in-flight long enough for the late joiner to attach.
iters=6000000

# `root=` teaches the stdio daemon's `UDSRemoteStore` where the machine1
# chroot store physically lives: a `UDSRemoteStore` is a `LocalFSStore`
# and serves `NarFromPath` straight from the real store dir, which is
# `/nix/store` itself in the deployed (non-chroot) topology. The `?` is
# percent-encoded so it survives the outer URI's query parsing.
builder="ssh-ng://localhost?remote-store=unix://$daemonSock%3Froot=$TEST_ROOT/machine1 - - 1 1"

buildOne() { # $1 = local store, $2 = output file
    nix build -L -f build-dedup-coordinator.nix slow \
        --arg busybox "$busybox" \
        --argstr marker "$marker" --argstr iters "$iters" \
        --no-link --max-jobs 0 \
        --store "$1" \
        --builders "$builder" > "$2" 2>&1
}

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

outA="$TEST_ROOT/fwd-dedup-A.out"
outB="$TEST_ROOT/fwd-dedup-B.out"

# 1. First client: its stdio daemon forwards the build to the shared
#    daemon, whose connection child relays to the (one) coordinator.
buildOne "$TEST_ROOT/machine0a" "$outA" &
pidA=$!

# 2. Wait until that build is in-flight (its marker has streamed to A).
waitFor "grepQuiet '$marker' '$outA'" 60 \
    || { echo "build A never started streaming" >&2; cat "$outA" >&2; exit 1; }

# 3. Second client, separate local store, separate SSH connection — so a
#    *separate* stdio daemon — same derivation.
buildOne "$TEST_ROOT/machine0b" "$outB" &
pidB=$!

# 4. The late joiner attaches through its own stdio daemon and the
#    coordinator replays the buffered log to it.
waitFor "grepQuiet '$marker' '$outB'" 60 \
    || { echo "late joiner never saw the replayed marker" >&2; cat "$outB" >&2; exit 1; }

# 5. Both builds finish.
wait "$pidA"; rA=$?
wait "$pidB"; rB=$?
[ "$rA" -eq 0 ] || { echo "build A failed" >&2; cat "$outA" >&2; exit 1; }
[ "$rB" -eq 0 ] || { echo "build B failed" >&2; cat "$outB" >&2; exit 1; }

# 6. Exactly one real build ran, despite two stdio daemons: same token.
tokA="$(sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$outA" | head -n1)"
tokB="$(sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$outB" | head -n1)"
echo "token A=$tokA  token B=$tokB" >&2
[ -n "$tokA" ] || { echo "no build token in A" >&2; cat "$outA" >&2; exit 1; }
[ "$tokA" = "$tokB" ] || { echo "tokens differ ⇒ two builds ran (no dedup across stdio daemons)" >&2; exit 1; }

# 7. Delegation, not degradation: nobody fell back to an uncoordinated
#    build (the unusable socket the stdio daemons inherited never came
#    into play).
grepQuietInverse "without build dedup" "$outA"
grepQuietInverse "without build dedup" "$outB"

echo "forwarded dedup OK: one build (token $tokA), two stdio daemons, one coordinator" >&2
