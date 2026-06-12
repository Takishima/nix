#!/usr/bin/env bash

# A wedged or dead coordinator must never wedge — or fail — the store's
# builds, and a clean coordinator exit must leave nothing behind:
#
#  A. Clean lifecycle: the election lock file doubles as the live
#     coordinator's pid file, and idle-exit removes the socket AND the
#     lock. (A stale root-owned lock under /nix/var/nix is what wedged
#     the deployed builder: it outlived every process and survived
#     daemon restarts.)
#
#  B. A wedged coordinator — alive and listening but never accepting
#     (here: SIGSTOPped): the relay's attach handshake must time out and
#     degrade to an uncoordinated build. Without the bound, the relay
#     blocks in read(2) forever: connect() succeeds into the listen
#     backlog and the request fits in the socket buffer, so the daemon
#     session hangs until the client disconnects ("interrupted by the
#     user" — the production signature).
#
#  C. The wedged holder lost its socket file but still holds the
#     election lock: every election is lost, and after its retry budget
#     the relay must degrade — not fail the build with "could not reach
#     the build coordinator".
#
#  D. A SIGKILLed coordinator (the NixOS-activation cgroup-teardown
#     shape: no cleanup code ran, socket and lock files both left
#     behind, flock released by the kernel): the next build must
#     re-elect promptly and stay fully coordinated — no degradation.
#
#  E. An ownerless listener squatting the socket path WITHOUT holding
#     the election lock (e.g. a process killed mid-teardown that kept
#     the bound socket alive): the fast-path connect keeps succeeding,
#     so the election never runs again and every build would pay the
#     attach timeout and degrade — forever, until something removes the
#     socket file by hand. The relay must steal the path instead: a
#     winnable lock proves the listener is ownerless, so it binds over
#     it and serves a fresh coordinator. Asserted as: the build stays
#     fully coordinated (no degradation warning).

source common.sh

TODO_NixOS

enableFeatures build-coordinator

clearStoreIfPossible

coordSock="$TEST_ROOT/wedged-coord.sock"
export NIX_BUILD_COORDINATOR_SOCKET="$coordSock"
# Keep the wedged-handshake bound short (default 30s).
export NIX_BUILD_COORDINATOR_ATTACH_TIMEOUT=2

rm -f "$coordSock" "$coordSock.lock"

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

build() { # $1 = derivation name, $2... = extra flags
    local name="$1"
    shift
    # `timeout` turns a pre-fix indefinite hang into a visible failure.
    timeout 120 nix build -f build-coordinator-wedged.nix --argstr name "$name" --no-link "$@" 2>&1
}

# --- A. clean lifecycle: pid file while alive, nothing left after -----------

out="$(build fresh-a)"
echo "$out" >&2
waitFor "[[ -s '$coordSock.lock' ]]" 10 \
    || { echo "the coordinator never wrote its pid into the lock file" >&2; exit 1; }
coordPid="$(tr -cd '0-9' < "$coordSock.lock")"
kill -0 "$coordPid" || { echo "the lock file does not name a live coordinator" >&2; exit 1; }

waitFor "[[ ! -e '$coordSock' && ! -e '$coordSock.lock' ]]" 60 \
    || { echo "coordinator exit left its socket or lock file behind" >&2; ls -la "$TEST_ROOT" >&2; exit 1; }

# --- B. wedged coordinator: bounded handshake, degraded build ---------------

out="$(build fresh-b)" # spawns a fresh coordinator
echo "$out" >&2
waitFor "[[ -s '$coordSock.lock' ]]" 10 \
    || { echo "the second coordinator never wrote its pid" >&2; exit 1; }
coordPid="$(tr -cd '0-9' < "$coordSock.lock")"
kill -0 "$coordPid" || { echo "the second coordinator is not alive" >&2; exit 1; }

# Alive, listening, never accepting: the wedge.
kill -STOP "$coordPid"
trap 'kill -KILL "$coordPid" 2>/dev/null || true' EXIT

if ! out="$(build wedged-b)"; then
    echo "build against a wedged coordinator failed or hung:" >&2
    echo "$out" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuiet "did not acknowledge"
echo "$out" | grepQuiet "without build dedup"

# --- C. held election lock, no socket: degrade after the retry budget -------

rm -f "$coordSock" # the stopped holder keeps the flock on the lock file

if ! out="$(build heldlock-c --option poll-interval 1)"; then
    echo "build with a held election lock failed instead of degrading:" >&2
    echo "$out" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuiet "without build dedup"

kill -KILL "$coordPid" 2>/dev/null || true
rm -f "$coordSock.lock"

# --- D. SIGKILLed coordinator: prompt re-election, no degradation ------------

out="$(build fresh-d)" # spawns a fresh coordinator
echo "$out" >&2
waitFor "[[ -s '$coordSock.lock' ]]" 10 \
    || { echo "the third coordinator never wrote its pid" >&2; exit 1; }
coordPid="$(tr -cd '0-9' < "$coordSock.lock")"

# No cleanup code runs; the socket and lock files stay behind, the flock
# is released by the kernel.
kill -KILL "$coordPid"
waitFor "! kill -0 '$coordPid' 2>/dev/null" 10

if ! out="$(build after-sigkill-d)"; then
    echo "build after a SIGKILLed coordinator failed:" >&2
    echo "$out" >&2
    exit 1
fi
echo "$out" >&2
# Full coordination via re-election over the stale files, not a fallback.
echo "$out" | grepQuietInverse "without build dedup"

# The re-elected coordinator must be a live, different process.
waitFor "[[ -s '$coordSock.lock' ]]" 10
newCoordPid="$(tr -cd '0-9' < "$coordSock.lock")"
[ "$newCoordPid" != "$coordPid" ] || { echo "lock file still names the killed coordinator" >&2; exit 1; }
kill -0 "$newCoordPid" || { echo "no live coordinator after re-election" >&2; exit 1; }

# --- E. ownerless squatter on the socket path: steal, don't degrade ---------

kill -KILL "$newCoordPid" 2>/dev/null || true
rm -f "$coordSock" "$coordSock.lock"

# A listener that owns the path but not the lock: a stopped `nix daemon`
# bound at the coordinator socket never accepts and never touches the
# lock file.
NIX_DAEMON_SOCKET_PATH="$coordSock" nix daemon --store "$TEST_ROOT/squat-store" >/dev/null 2>&1 &
squatPid=$!
trap 'kill -KILL "$coordPid" "$squatPid" 2>/dev/null || true' EXIT
waitFor "[[ -S '$coordSock' ]]" 30 \
    || { echo "the squatter never bound the socket path" >&2; exit 1; }
kill -STOP "$squatPid"

if ! out="$(build squatter-e)"; then
    echo "build against a squatted socket path failed:" >&2
    echo "$out" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuietInverse "without build dedup"
waitFor "[[ -s '$coordSock.lock' ]]" 10 \
    || { echo "no coordinator took over the squatted path" >&2; exit 1; }
kill -0 "$(tr -cd '0-9' < "$coordSock.lock")" \
    || { echo "the lock file does not name a live coordinator after the steal" >&2; exit 1; }

kill -KILL "$squatPid" 2>/dev/null || true

echo "wedged-coordinator degradation OK" >&2
