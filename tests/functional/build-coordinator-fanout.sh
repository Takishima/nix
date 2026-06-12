#!/usr/bin/env bash

# N-way fan-out/replay fidelity on the forwarded (daemon-relay) topology —
# the shape the deployed ssh-ng builders exercise. The two-client dedup
# tests prove "the late joiner saw one replayed marker"; this proves the
# feature's actual promise at scale:
#
#  * 12 clients on one slow build — 3 racing the very first
#    START_OR_ATTACH (only one may win the registry), 9 attaching at
#    staggered offsets through the build;
#  * the build emits ~3 MB of deterministic mixed-length output in one
#    burst, faster than any subscriber drains it — under the replay caps
#    (1 MiB head + 3 MiB tail), so NO attacher may legitimately see
#    truncation;
#  * per client: exit 0 like the owner, the received build log
#    byte-identical to the owner's (same content, same order — compared
#    by digest, not by grep), exactly ONE token line and it matches the
#    owner's (a second token line = a duplicated replay; a different
#    token = a second execution);
#  * globally: no degradation warning anywhere.
#
# Then the attach-vs-finish window, which a single attempt cannot catch:
# 20 rounds of a short fresh build with one attacher launched at
# randomized jitter around completion — both orderings of that race. An
# attacher that lands while the build finishes must either attach (same
# token) or miss and find the output already valid (no token at all);
# a *different* token is the off-by-one between "replay finished build"
# and "start new build": a duplicated execution.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/fan-"* || true

daemonSock="$TEST_ROOT/fan-daemon.sock"

# The shared daemon: the only process with a usable coordinator location;
# its connection children run the daemon-level relay.
NIX_DAEMON_SOCKET_PATH="$daemonSock" \
    NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/fan-coord.sock" \
    nix daemon --store "$TEST_ROOT/machine1" &
daemonPid=$!
trap 'kill "$daemonPid" 2>/dev/null || true' EXIT

for ((i = 0; i < 100; i++)); do
    if [[ -S "$daemonSock" ]]; then break; fi
    kill -0 "$daemonPid" || { echo "shared daemon died" >&2; exit 1; }
    sleep 0.1
done
[[ -S "$daemonSock" ]] || { echo "shared daemon never bound its socket" >&2; exit 1; }

# The clients (and through fake-SSH their stdio daemons) must delegate,
# not coordinate; an election on their side would surface as the
# degradation warning asserted absent below.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/no-such-dir/coord.sock"

# See build-dedup-coordinator-forwarded.sh for the root= rationale.
builder="ssh-ng://localhost?remote-store=unix://$daemonSock%3Froot=$TEST_ROOT/machine1 - - 1 1"

marker="fanout-marker-$$"

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

buildOne() { # $1 = client id, $2 = drv name, $3 = holdIters, $4 = bulkLines, $5 = out file
    timeout 240 nix build -L -f build-coordinator-fanout.nix \
        --arg busybox "$busybox" --argstr marker "$marker" \
        --argstr name "$2" --argstr holdIters "$3" --argstr bulkLines "$4" \
        --no-link --max-jobs 0 \
        --store "$TEST_ROOT/machine0-$1" \
        --builders "$builder" > "$5" 2>&1
}

tokenOf() { # $1 = out file
    sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$1" | head -n1
}

# --- phase 1: 12-way fan-out, byte fidelity ---------------------------------

# ~8M iterations ≈ 18s in-flight before the bulk: ample for the staggered
# attachers. 15000 lines × ~200 B ≈ 3 MB — inside the 4 MiB replay caps.
hold=8000000
lines=15000

declare -a pids outs

# Three clients race the initial registration.
for c in 0 1 2; do
    outs[c]="$TEST_ROOT/fan-$c.out"
    buildOne "$c" fanout-big "$hold" "$lines" "${outs[c]}" &
    pids[c]=$!
done

waitFor "grepQuiet '$marker' '${outs[0]}'" 120 \
    || { echo "the build never started streaming" >&2; cat "${outs[0]}" >&2; exit 1; }

# Nine more, staggered through the hold window.
for ((c = 3; c < 12; c++)); do
    sleep 0.3
    outs[c]="$TEST_ROOT/fan-$c.out"
    buildOne "$c" fanout-big "$hold" "$lines" "${outs[c]}" &
    pids[c]=$!
done

for ((c = 0; c < 12; c++)); do
    wait "${pids[c]}" \
        || { echo "client $c failed where the owner succeeded" >&2; cat "${outs[c]}" >&2; exit 1; }
done

refTok="$(tokenOf "${outs[0]}")"
[ -n "$refTok" ] || { echo "no build token in the owner's log" >&2; cat "${outs[0]}" >&2; exit 1; }
refDigest="$(grep -o 'NLOG.*' "${outs[0]}" | sha256sum | cut -d' ' -f1)"
refCount="$(grep -c 'NLOG' "${outs[0]}" || true)"
[ "$refCount" -eq $((lines + 1)) ] \
    || { echo "the owner's log is incomplete ($refCount of $((lines + 1)) NLOG lines)" >&2; exit 1; }

for ((c = 0; c < 12; c++)); do
    out="${outs[c]}"
    grepQuietInverse "without build dedup" "$out" \
        || { echo "client $c degraded" >&2; cat "$out" >&2; exit 1; }
    tokCount="$(grep -c 'BUILDTOKEN:' "$out" || true)"
    [ "$tokCount" -eq 1 ] \
        || { echo "client $c saw $tokCount token lines (want exactly 1: >1 = duplicated replay, 0 = no attach)" >&2; cat "$out" >&2; exit 1; }
    tok="$(tokenOf "$out")"
    [ "$tok" = "$refTok" ] \
        || { echo "client $c saw token $tok, owner saw $refTok: a second execution ran" >&2; exit 1; }
    digest="$(grep -o 'NLOG.*' "$out" | sha256sum | cut -d' ' -f1)"
    [ "$digest" = "$refDigest" ] \
        || { echo "client $c's log is not byte-identical to the owner's" >&2; diff <(grep -o 'NLOG.*' "${outs[0]}") <(grep -o 'NLOG.*' "$out") | head -20 >&2; exit 1; }
done

echo "fan-out fidelity OK: 12 clients, one execution ($refTok), identical logs" >&2

# --- phase 2: the attach-vs-finish window ------------------------------------

# A short fresh build per round; the attacher lands at randomized jitter
# around its completion, hitting both orderings of the race across rounds.
for ((r = 0; r < 20; r++)); do
    nm="fanout-win-$r"
    oOut="$TEST_ROOT/fan-win-$r-owner.out"
    aOut="$TEST_ROOT/fan-win-$r-attacher.out"

    buildOne w "$nm" 400000 30 "$oOut" &
    oPid=$!
    waitFor "grepQuiet '$marker' '$oOut'" 60 \
        || { echo "round $r: the owner never started streaming" >&2; cat "$oOut" >&2; exit 1; }

    jitter=$((RANDOM % 19)) # 0.0–1.8s; the build body runs ~1s
    sleep "$((jitter / 10)).$((jitter % 10))"

    buildOne x "$nm" 400000 30 "$aOut" &
    aPid=$!

    wait "$oPid" || { echo "round $r: the owner failed" >&2; cat "$oOut" >&2; exit 1; }
    wait "$aPid" || { echo "round $r: the attacher failed" >&2; cat "$aOut" >&2; exit 1; }

    grepQuietInverse "without build dedup" "$oOut" \
        || { echo "round $r: the owner degraded" >&2; cat "$oOut" >&2; exit 1; }
    grepQuietInverse "without build dedup" "$aOut" \
        || { echo "round $r: the attacher degraded" >&2; cat "$aOut" >&2; exit 1; }

    oTok="$(tokenOf "$oOut")"
    [ -n "$oTok" ] || { echo "round $r: no token in the owner's log" >&2; cat "$oOut" >&2; exit 1; }
    aTokCount="$(grep -c 'BUILDTOKEN:' "$aOut" || true)"
    if [ "$aTokCount" -gt 1 ]; then
        echo "round $r: the attacher saw $aTokCount token lines (resolved twice?)" >&2
        cat "$aOut" >&2
        exit 1
    elif [ "$aTokCount" -eq 1 ]; then
        aTok="$(tokenOf "$aOut")"
        [ "$aTok" = "$oTok" ] \
            || { echo "round $r: attacher token $aTok != owner token $oTok: a second execution raced the finish" >&2; exit 1; }
    fi # 0 tokens: the attacher missed the build and found the output valid — fine.
done

echo "attach-vs-finish window OK: 20 rounds, no duplicated execution" >&2
