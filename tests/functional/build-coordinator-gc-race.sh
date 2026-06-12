#!/usr/bin/env bash

# Garbage collection racing a coordinated build. The coordinator's socket
# and election lock live in the store's own state dir (no env override
# here — this test wants the DEFAULT `$stateDir/coordinator.socket`, the
# tree GC walks for roots and temproots), and GC takes the global GC lock
# the coordinator's build children also interact with via temproots.
#
# Three GC points, all against the builder store while it hosts the
# coordinator:
#   1. build in flight with an attacher: the build's inputs and
#      temporaries must survive (normal temproot semantics through the
#      coordinator's build child), GC must complete (no deadlock with the
#      coordinator), and the socket/lock files must not be collected;
#   2. GC racing the registration of a fresh build;
#   3. GC concurrent with a post-completion late client (the
#      replay/AlreadyValid edge).
# After all three: a fresh build must still be fully coordinated — the
# files GC walked past must still name a usable coordinator.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

enableFeatures build-coordinator

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR
# The point of this test is the DEFAULT socket location inside the
# builder store's state dir.
unset NIX_BUILD_COORDINATOR_SOCKET

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/gcrace-"* || true

# Top-level builds on the ssh-ng store: the fake-SSH daemon serves the
# machine1 chroot store, its goal-level relay elects the coordinator at
# machine1's state dir.
store="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1"
coordSock="$TEST_ROOT/machine1/nix/var/nix/coordinator.socket"

marker="gcrace-marker-$$"
iters=6000000 # shell-only busybox: builtin loop holds the build in-flight

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(($2 * 5))" ] || return 1
    done
}

buildOne() { # $1 = drv name, $2 = iters, $3 = out file
    timeout 240 nix build -L -f build-coordinator-gc-race.nix \
        --arg busybox "$busybox" --argstr marker "$marker" \
        --argstr iters "$2" --argstr name "$1" \
        --no-link --store "$store" > "$3" 2>&1
}

tokenOf() { # $1 = out file
    sed -n 's/.*BUILDTOKEN:\([0-9a-f-]*\).*/\1/p' "$1" | head -n1
}

runGC() { # $1 = log file
    # Bounded: a deadlock between the GC lock and the coordinator is
    # exactly what this would turn into.
    timeout 120 nix store gc --store "$TEST_ROOT/machine1" > "$1" 2>&1
}

# --- 1. GC during an in-flight coordinated build with an attacher ------------

outA="$TEST_ROOT/gcrace-A.out"
outB="$TEST_ROOT/gcrace-B.out"

buildOne gcrace-1 "$iters" "$outA" &
pidA=$!
waitFor "grepQuiet '$marker' '$outA'" 120 \
    || { echo "the build never started streaming" >&2; cat "$outA" >&2; exit 1; }

buildOne gcrace-1 "$iters" "$outB" &
pidB=$!
waitFor "grepQuiet 'BUILDTOKEN:' '$outB'" 60 \
    || { echo "the attacher never saw the replayed token" >&2; cat "$outB" >&2; exit 1; }

[ -S "$coordSock" ] || { echo "no coordinator socket in the state dir" >&2; exit 1; }
[ -e "$coordSock.lock" ] || { echo "no coordinator lock in the state dir" >&2; exit 1; }

runGC "$TEST_ROOT/gcrace-gc1.log" \
    || { echo "GC failed or deadlocked during the in-flight build" >&2; cat "$TEST_ROOT/gcrace-gc1.log" >&2; exit 1; }

# GC walked the state dir; the coordinator's files must have survived it.
[ -S "$coordSock" ] || { echo "GC collected the coordinator socket" >&2; exit 1; }
[ -e "$coordSock.lock" ] || { echo "GC collected the coordinator lock" >&2; exit 1; }

wait "$pidA" || { echo "the build failed under concurrent GC" >&2; cat "$outA" >&2; exit 1; }
wait "$pidB" || { echo "the attacher failed under concurrent GC" >&2; cat "$outB" >&2; exit 1; }

tokA="$(tokenOf "$outA")"
tokB="$(tokenOf "$outB")"
[ -n "$tokA" ] || { echo "no token in the owner's log" >&2; cat "$outA" >&2; exit 1; }
[ "$tokA" = "$tokB" ] || { echo "tokens differ: GC broke the coalescing" >&2; exit 1; }
grepQuietInverse "without build dedup" "$outA"
grepQuietInverse "without build dedup" "$outB"

# --- 2. GC racing the registration of a fresh build --------------------------

outC="$TEST_ROOT/gcrace-C.out"
buildOne gcrace-2 1000000 "$outC" &
pidC=$!
runGC "$TEST_ROOT/gcrace-gc2.log" \
    || { echo "GC failed while racing a build's registration" >&2; cat "$TEST_ROOT/gcrace-gc2.log" >&2; exit 1; }
wait "$pidC" || { echo "the build failed with GC racing its registration" >&2; cat "$outC" >&2; exit 1; }
grepQuiet 'BUILDTOKEN:' "$outC" || { echo "the racing-GC build was not coordinated" >&2; cat "$outC" >&2; exit 1; }
grepQuietInverse "without build dedup" "$outC"

# --- 3. GC concurrent with a post-completion late client ---------------------

outD="$TEST_ROOT/gcrace-D.out"
outE="$TEST_ROOT/gcrace-E.out"
buildOne gcrace-3 1000000 "$outD" \
    || { echo "the pre-GC build failed" >&2; cat "$outD" >&2; exit 1; }

# Immediately behind the completion: a late client (replay or
# AlreadyValid, depending on the race — both must work) and GC at once.
buildOne gcrace-3 1000000 "$outE" &
pidE=$!
runGC "$TEST_ROOT/gcrace-gc3.log" \
    || { echo "GC failed during the post-completion window" >&2; cat "$TEST_ROOT/gcrace-gc3.log" >&2; exit 1; }
wait "$pidE" || { echo "the late client failed under concurrent GC" >&2; cat "$outE" >&2; exit 1; }
grepQuietInverse "without build dedup" "$outE"

# --- and the coordinator the GCs walked past is still usable ------------------

outF="$TEST_ROOT/gcrace-F.out"
buildOne gcrace-4 1000000 "$outF" \
    || { echo "the post-GC build failed" >&2; cat "$outF" >&2; exit 1; }
grepQuiet 'BUILDTOKEN:' "$outF" || { echo "the post-GC build was not coordinated" >&2; cat "$outF" >&2; exit 1; }
grepQuietInverse "without build dedup" "$outF"

echo "GC-vs-coordinator OK: three GC points, builds coordinated throughout, state files intact" >&2
