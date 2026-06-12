#!/usr/bin/env bash

# Failure fan-out at scale on the forwarded (daemon-relay) topology: one
# failing build, 8 clients attached across the same stagger pattern as the
# success fan-out (2 racing the registration, 6 staggered mid-build).
#
# Per client: fails exactly ONCE (a duplicated token line would be a
# waiter resolved twice), with the build's real exit semantics (the
# builder's `exit 7` surfaces in the message), and the failure log —
# token and last-words line — fanned out or replayed to it. Globally:
# one execution (same token everywhere), no degradation warning.
#
# A client arriving AFTER the failure completed gets a FRESH execution
# (different token, fails the same way): the registry deliberately never
# reuses a finished result — nix has no negative caching, and a stale
# transient failure handed to a new requester would be wrong. What this
# pins is that the fresh execution is exactly one orderly re-run, not a
# replayed corpse and not an execution storm. Finally a fresh derivation
# must build cleanly: no poisoned daemon or coordinator.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/failfan-"* || true

daemonSock="$TEST_ROOT/failfan-daemon.sock"

NIX_DAEMON_SOCKET_PATH="$daemonSock" \
    NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/failfan-coord.sock" \
    nix daemon --store "$TEST_ROOT/machine1" &
daemonPid=$!
trap 'kill "$daemonPid" 2>/dev/null || true' EXIT

for ((i = 0; i < 100; i++)); do
    if [[ -S "$daemonSock" ]]; then break; fi
    kill -0 "$daemonPid" || { echo "shared daemon died" >&2; exit 1; }
    sleep 0.1
done
[[ -S "$daemonSock" ]] || { echo "shared daemon never bound its socket" >&2; exit 1; }

export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/coord.sock"

# See build-dedup-coordinator-forwarded.sh for the root= rationale.
builder="ssh-ng://localhost?remote-store=unix://$daemonSock%3Froot=$TEST_ROOT/machine1 - - 1 1"

marker="failfan-marker-$$"
iters=4000000 # ~9s in-flight: covers the stagger comfortably

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

buildOne() { # $1 = client id, $2 = attr, $3 = out file
    timeout 240 nix build -L -f build-coordinator-failure-fanout.nix "$2" \
        --arg busybox "$busybox" --argstr marker "$marker" \
        --argstr iters "$iters" --argstr name failfan \
        --no-link --max-jobs 0 \
        --store "$TEST_ROOT/machine0-$1" \
        --builders "$builder" > "$3" 2>&1
}

tokenOf() { # $1 = out file
    sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$1" | head -n1
}

declare -a pids outs

# Two clients race the registration...
for c in 0 1; do
    outs[c]="$TEST_ROOT/failfan-$c.out"
    buildOne "$c" failing "${outs[c]}" &
    pids[c]=$!
done

waitFor "grepQuiet '$marker' '${outs[0]}'" 120 \
    || { echo "the failing build never started streaming" >&2; cat "${outs[0]}" >&2; exit 1; }

# ...six more attach staggered while it is in flight.
for ((c = 2; c < 8; c++)); do
    sleep 0.4
    outs[c]="$TEST_ROOT/failfan-$c.out"
    buildOne "$c" failing "${outs[c]}" &
    pids[c]=$!
done

for ((c = 0; c < 8; c++)); do
    if wait "${pids[c]}"; then
        echo "client $c SUCCEEDED on a failing build" >&2
        cat "${outs[c]}" >&2
        exit 1
    fi
done

refTok="$(tokenOf "${outs[0]}")"
[ -n "$refTok" ] || { echo "no build token in client 0's log" >&2; cat "${outs[0]}" >&2; exit 1; }

for ((c = 0; c < 8; c++)); do
    out="${outs[c]}"
    grepQuietInverse "without build dedup" "$out" \
        || { echo "client $c degraded" >&2; cat "$out" >&2; exit 1; }
    # Count LIVE token lines only (drv-name prefixed): the fail-loud
    # error summary quotes the last log lines again ("> BUILDTOKEN:..."),
    # which is UX, not a second resolution.
    tokCount="$(grep -c 'failfan> BUILDTOKEN:' "$out" || true)"
    [ "$tokCount" -eq 1 ] \
        || { echo "client $c saw $tokCount live token lines (a waiter resolved twice?)" >&2; cat "$out" >&2; exit 1; }
    tok="$(tokenOf "$out")"
    [ "$tok" = "$refTok" ] \
        || { echo "client $c saw token $tok, expected $refTok: a second execution ran" >&2; exit 1; }
    grepQuiet 'FAILLINE:last words before death' "$out" \
        || { echo "client $c never saw the failure log" >&2; cat "$out" >&2; exit 1; }
    grepQuiet 'exit code 7' "$out" \
        || { echo "client $c lost the real exit semantics" >&2; cat "$out" >&2; exit 1; }
done

echo "failure fan-out OK: 8 clients, one execution ($refTok), each failed exactly once" >&2

# --- a client arriving after the failure completed ---------------------------

lateOut="$TEST_ROOT/failfan-late.out"
if buildOne late failing "$lateOut"; then
    echo "the late client SUCCEEDED on a failing build" >&2
    cat "$lateOut" >&2
    exit 1
fi
grepQuietInverse "without build dedup" "$lateOut" \
    || { echo "the late client degraded" >&2; cat "$lateOut" >&2; exit 1; }
lateCount="$(grep -c 'failfan> BUILDTOKEN:' "$lateOut" || true)"
[ "$lateCount" -eq 1 ] \
    || { echo "the late client saw $lateCount live token lines (want exactly one fresh execution)" >&2; cat "$lateOut" >&2; exit 1; }
lateTok="$(tokenOf "$lateOut")"
[ "$lateTok" != "$refTok" ] \
    || { echo "the late client was handed the finished failure instead of a fresh execution" >&2; exit 1; }
grepQuiet 'exit code 7' "$lateOut" \
    || { echo "the late client lost the real exit semantics" >&2; cat "$lateOut" >&2; exit 1; }

# --- and the daemon is not poisoned ------------------------------------------

okOut="$TEST_ROOT/failfan-ok.out"
buildOne fresh ok "$okOut" \
    || { echo "a fresh build failed after the failure storm" >&2; cat "$okOut" >&2; exit 1; }

echo "failure semantics OK: late arrival re-ran fresh, fresh build clean" >&2
