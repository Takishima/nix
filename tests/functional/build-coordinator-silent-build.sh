#!/usr/bin/env bash

# A coordinated build whose log goes silent for a long stretch while it is
# still running must NOT be killed — for every attached subscriber — on the
# forwarded (daemon-relay) topology the deployed ssh-ng builders use.
#
# Regression for the field report "coordinated ssh-ng build dies ~4 min
# into log silence". The build prints a marker, then emits no log bytes for
# a window that comfortably exceeds every bound in the coordinator/relay
# code (the 30 s attach handshake and the ~20 s idle-exit tick — and
# idle-exit cannot even arm while a build is running), then completes. Owner
# and a mid-silence attacher must both finish, coalesced onto one execution,
# with no degradation warning and no daemon disconnect.
#
# The silence window is `COORD_SILENT_ITERS` busy-loop iterations (~450k/s
# in this sandbox shell); the fleet can dial it to minutes. The default is
# sized to clear the in-code bounds with wide margin while keeping the
# suite runtime bounded.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/silent-"* || true

# ~60 s of pure silence by default: 2x the 30 s attach bound and 3x the
# ~20 s idle tick. Verified directly against the field report's window: a
# 145M-iteration (~320 s) run completes cleanly here, so the fork has no
# coordinator-internal silence bound below several minutes. Override to
# probe longer windows.
iters="${COORD_SILENT_ITERS:-27000000}"

daemonSock="$TEST_ROOT/silent-daemon.sock"

NIX_DAEMON_SOCKET_PATH="$daemonSock" \
    NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/silent-coord.sock" \
    nix daemon --store "$TEST_ROOT/machine1" &
daemonPid=$!
trap 'kill "$daemonPid" 2>/dev/null || true' EXIT

for ((i = 0; i < 100; i++)); do
    if [[ -S "$daemonSock" ]]; then break; fi
    kill -0 "$daemonPid" || { echo "shared daemon died" >&2; exit 1; }
    sleep 0.1
done
[[ -S "$daemonSock" ]] || { echo "shared daemon never bound its socket" >&2; exit 1; }

# The clients (and through fake-SSH their stdio daemons) delegate, not
# coordinate; an election on their side would warn "without build dedup".
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/coord.sock"

# See build-dedup-coordinator-forwarded.sh for the root= rationale.
builder="ssh-ng://localhost?remote-store=unix://$daemonSock%3Froot=$TEST_ROOT/machine1 - - 1 1"

marker="silent-marker-$$"

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

buildOne() { # $1 = client id, $2 = out file
    # No `timeout`: a wrongly-applied silence bound is the bug under test;
    # the meson per-test timeout still bounds a true hang.
    nix build -L -f build-coordinator-silent-build.nix \
        --arg busybox "$busybox" --argstr marker "$marker" \
        --argstr name silent-build --argstr iters "$iters" \
        --no-link --max-jobs 0 \
        --store "$TEST_ROOT/machine0-$1" \
        --builders "$builder" > "$2" 2>&1
}

tokenOf() { # $1 = out file
    sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$1" | head -n1
}

outA="$TEST_ROOT/silent-A.out"
outB="$TEST_ROOT/silent-B.out"

# Owner starts the (only) build and holds it silent.
buildOne A "$outA" &
pidA=$!
waitFor "grepQuiet '$marker' '$outA'" 120 \
    || { echo "the build never started streaming" >&2; cat "$outA" >&2; exit 1; }

# Attacher joins while the build is mid-silence (its token already streamed,
# its log now quiet).
buildOne B "$outB" &
pidB=$!
waitFor "grepQuiet 'BUILDTOKEN:' '$outB'" 60 \
    || { echo "the attacher never saw the replayed token" >&2; cat "$outB" >&2; exit 1; }

# Both must survive the silence and complete.
wait "$pidA" || { echo "the owner died during the silent window (the reported bug)" >&2; cat "$outA" >&2; exit 1; }
wait "$pidB" || { echo "the attacher died during the silent window (the reported bug)" >&2; cat "$outB" >&2; exit 1; }

# DONE on both (the post-silence output streamed/replayed through).
grepQuiet 'silent-build-DONE' "$outA" || { echo "the owner never reached DONE" >&2; cat "$outA" >&2; exit 1; }
grepQuiet 'silent-build-DONE' "$outB" || { echo "the attacher never reached DONE" >&2; cat "$outB" >&2; exit 1; }

# One execution, no degradation.
tokA="$(tokenOf "$outA")"
tokB="$(tokenOf "$outB")"
[ -n "$tokA" ] || { echo "no token in the owner's log" >&2; cat "$outA" >&2; exit 1; }
[ "$tokA" = "$tokB" ] || { echo "tokens differ: the silent build was not coalesced" >&2; exit 1; }
grepQuietInverse "without build dedup" "$outA"
grepQuietInverse "without build dedup" "$outB"

echo "silent-build OK: a build silent for $iters iterations stayed coordinated for both clients (token $tokA)" >&2
