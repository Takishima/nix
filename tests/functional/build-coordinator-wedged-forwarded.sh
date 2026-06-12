#!/usr/bin/env bash

# Wedge recovery on the *forwarded* (daemon-relay) topology — the path the
# deployed ssh-ng builders exercise: stdio daemons forward BuildDerivation
# to a shared daemon whose connection children relay to the coordinator
# (`relayBuildToCoordinator`). After a coordinator dies without cleanup
# (SIGKILL — the NixOS activation cgroup-teardown shape), wedges (stopped),
# or an ownerless listener squats the socket path, the next forwarded
# build must work promptly with NO external cleanup of the coordinator
# socket or lock.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/fwdw-"* "$TEST_ROOT/squat-store" || true

daemonSock="$TEST_ROOT/fwdw-daemon.sock"
coordSock="$TEST_ROOT/fwdw-coord.sock"

# The shared daemon: the only process with a usable coordinator location;
# its connection children run the daemon-level relay. Short attach bound
# so the wedged rounds don't wait the production default.
NIX_DAEMON_SOCKET_PATH="$daemonSock" \
    NIX_BUILD_COORDINATOR_SOCKET="$coordSock" \
    NIX_BUILD_COORDINATOR_ATTACH_TIMEOUT=2 \
    nix daemon --store "$TEST_ROOT/machine1" &
daemonPid=$!
trap 'kill "$daemonPid" 2>/dev/null || true; kill -KILL "${coordPid-}" "${squatPid-}" 2>/dev/null || true' EXIT

for ((i = 0; i < 100; i++)); do
    if [[ -S "$daemonSock" ]]; then break; fi
    kill -0 "$daemonPid" || { echo "shared daemon died" >&2; exit 1; }
    sleep 0.1
done
[[ -S "$daemonSock" ]] || { echo "shared daemon never bound its socket" >&2; exit 1; }

# The clients (and through fake-SSH their stdio daemons) must delegate, not
# coordinate: an election on their side would surface as a degradation
# warning, asserted absent below.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/coord.sock"

# See build-dedup-coordinator-forwarded.sh for the root= rationale.
builder="ssh-ng://localhost?remote-store=unix://$daemonSock%3Froot=$TEST_ROOT/machine1 - - 1 1"

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

build() { # $1 = derivation name
    local name="$1"
    timeout 120 nix build -L -f build-coordinator-wedged-forwarded.nix \
        --arg busybox "$busybox" --argstr name "$name" \
        --no-link --max-jobs 0 \
        --store "$TEST_ROOT/machine0" \
        --builders "$builder" 2>&1
}

# --- 1. baseline: forwarded build spawns the shared daemon's coordinator ----

if ! out="$(build fresh-1)"; then
    echo "$out" >&2
    echo "baseline forwarded build failed" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuietInverse "without build dedup"
waitFor "[[ -s '$coordSock.lock' ]]" 10 \
    || { echo "the coordinator never wrote its pid" >&2; exit 1; }
coordPid="$(tr -cd '0-9' < "$coordSock.lock")"
kill -0 "$coordPid" || { echo "the lock file does not name a live coordinator" >&2; exit 1; }

# --- 2. SIGKILLed coordinator: prompt re-election, still coordinated --------

kill -KILL "$coordPid"
waitFor "! kill -0 '$coordPid' 2>/dev/null" 10

if ! out="$(build after-sigkill-2)"; then
    echo "$out" >&2
    echo "forwarded build after a SIGKILLed coordinator failed or hung" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuietInverse "without build dedup"

# --- 3. wedged (stopped) coordinator: bounded degrade, build succeeds -------

waitFor "[[ -s '$coordSock.lock' ]]" 10 \
    || { echo "no coordinator after re-election" >&2; exit 1; }
coordPid="$(tr -cd '0-9' < "$coordSock.lock")"
kill -0 "$coordPid" || { echo "re-elected coordinator is not alive" >&2; exit 1; }
kill -STOP "$coordPid"

if ! out="$(build wedged-3)"; then
    echo "$out" >&2
    echo "forwarded build against a wedged coordinator failed or hung" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuiet "without build dedup"
kill -KILL "$coordPid"

# --- 4. ownerless squatter on the socket path: the relay steals it ----------

rm -f "$coordSock" "$coordSock.lock"
NIX_DAEMON_SOCKET_PATH="$coordSock" nix daemon --store "$TEST_ROOT/squat-store" >/dev/null 2>&1 &
squatPid=$!
waitFor "[[ -S '$coordSock' ]]" 30 \
    || { echo "the squatter never bound the socket path" >&2; exit 1; }
kill -STOP "$squatPid"

if ! out="$(build squatter-4)"; then
    echo "$out" >&2
    echo "forwarded build against a squatted socket path failed" >&2
    exit 1
fi
echo "$out" >&2
echo "$out" | grepQuietInverse "without build dedup"
kill -KILL "$squatPid"

echo "forwarded wedge recovery OK" >&2
