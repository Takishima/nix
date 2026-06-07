#!/usr/bin/env bash

# RFC `remote-build-protocol-redesign` Phase 3 (G3): the build coordinator
# coalesces two concurrent builds of the *same resolved derivation* into one
# build, fans its log out to both clients, and replays the buffered log to the
# late joiner.
#
# Two independent `nix build` invocations (separate local stores, so their
# output PathLocks don't pre-coalesce them) each offload the *same* derivation
# to one `ssh-ng://` builder via the build hook (`--max-jobs 0`). The builder
# daemon has NIX_BUILD_COORDINATOR_SOCKET set, so its `BuildDerivation` handler
# relays to a single per-store coordinator. The first request starts the build;
# the second is a registry HIT that attaches to it and replays its log — exactly
# one real build runs.
#
# fake-SSH to localhost (no sshd): `ssh-ng://localhost` runs the remote daemon
# as a local subprocess that inherits this test's environment (so it sees the
# coordinator socket). The build blocks on a FIFO until we release it, which
# keeps it in-flight long enough for the second request to attach — no timing
# races: we detect the attach by waiting for the replayed marker in the late
# joiner's output, then assert exactly one build ran before releasing.

source common.sh

TODO_NixOS

# busybox is the statically-linked builder for the isolated remote store.
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/dedup-"* "$TEST_ROOT/coord."* || true

count="$TEST_ROOT/dedup-count"     # one line appended per *real* build
started="$TEST_ROOT/dedup-started" # touched when a build is in-flight
fifo="$TEST_ROOT/dedup-fifo"       # build blocks on this until released
marker="build-dedup-marker-$$"
mkfifo "$fifo"

# The builder daemon (fake-SSH localhost subprocess) inherits this and relays
# BuildDerivation to a single per-store coordinator.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord.sock"

builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

buildOne() { # $1 = local store, $2 = output file
    nix build -L -f build-dedup-coordinator.nix slow \
        --arg busybox "$busybox" \
        --argstr count "$count" --argstr started "$started" \
        --argstr fifo "$fifo" --argstr marker "$marker" \
        --no-link --max-jobs 0 \
        --store "$1" \
        --builders "$builder" > "$2" 2>&1
}

waitFor() { # $1 = predicate cmd, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

# 1. First builder: starts the (only) build, then blocks on the FIFO.
buildOne "$TEST_ROOT/machine0a" "$TEST_ROOT/dedup-A.out" &
pidA=$!

# 2. Wait until that build is actually in-flight in the coordinator.
waitFor "[ -e '$started' ]" 60 || { echo "build A never started" >&2; cat "$TEST_ROOT/dedup-A.out" >&2; exit 1; }

# 3. Second, concurrent build of the *same* derivation, separate local store.
buildOne "$TEST_ROOT/machine0b" "$TEST_ROOT/dedup-B.out" &
pidB=$!

# 4. The late joiner attaches and the coordinator *replays* the buffered log to
#    it: wait for the marker to surface in B's output. This is the attach proof
#    and avoids any timing race (the build is still blocked on the FIFO).
waitFor "grepQuiet '$marker' '$TEST_ROOT/dedup-B.out'" 60 \
    || { echo "late joiner never saw the replayed marker" >&2; cat "$TEST_ROOT/dedup-B.out" >&2; exit 1; }

# 5. Exactly one *real* build ran — the second request coalesced (dedup, G3).
nbuilds="$(wc -l < "$count")"
echo "real builds: $nbuilds (expected 1)" >&2
[ "$nbuilds" -eq 1 ]

# 6. Release the shared build; both clients get the result.
echo go > "$fifo"

wait "$pidA"; rA=$?
wait "$pidB"; rB=$?
[ "$rA" -eq 0 ] || { echo "build A failed" >&2; cat "$TEST_ROOT/dedup-A.out" >&2; exit 1; }
[ "$rB" -eq 0 ] || { echo "build B failed" >&2; cat "$TEST_ROOT/dedup-B.out" >&2; exit 1; }

# 7. Both clients saw the (one) build's log — A live, B via replay + live tail.
grepQuiet "$marker" "$TEST_ROOT/dedup-A.out"
grepQuiet "$marker" "$TEST_ROOT/dedup-B.out"

# 8. Still exactly one build after completion (no late second build).
[ "$(wc -l < "$count")" -eq 1 ]

echo "dedup + replay + fan-out OK: one build, two clients, late joiner replayed" >&2
