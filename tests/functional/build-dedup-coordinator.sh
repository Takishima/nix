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
# the second is a registry HIT that attaches and replays its log.
#
# The build runs in an isolated mount namespace, so it can only communicate via
# its streamed log. Each *real* build prints a token unique to its builder
# process (the shell PID). The test asserts BOTH clients observe the SAME token:
# that is only possible if exactly one build ran and its log was fanned out —
# the late joiner receiving the pre-attach lines via replay. Different tokens
# would mean two separate builds (no dedup).
#
# fake-SSH to localhost (no sshd): `ssh-ng://localhost` runs the remote daemon
# as a local subprocess that inherits this test's environment (so it sees the
# coordinator socket). The build stays in-flight for `seconds` so the second
# request reliably attaches while the first is still running.

source common.sh

TODO_NixOS

# busybox is the statically-linked builder for the isolated remote store.
[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

# Avoid the store dir being inside a build dir.
unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* "$TEST_ROOT/coord."* || true

marker="build-dedup-marker-$$"
# Busybox here is shell-only (no `sleep`), so the builder burns wall-clock with
# a builtin loop to stay in-flight. ~444K iters/s in busybox ash on this CI, so
# ~6M ≈ 13s — comfortably longer than the late joiner's attach, which is what
# the test actually waits for.
iters=6000000

# The builder daemon (fake-SSH localhost subprocess) inherits this and relays
# BuildDerivation to a single per-store coordinator.
export NIX_BUILD_COORDINATOR_SOCKET="$TEST_ROOT/coord.sock"

builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1"

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

outA="$TEST_ROOT/dedup-A.out"
outB="$TEST_ROOT/dedup-B.out"

# 1. First builder: starts the (only) build and holds it in-flight.
buildOne "$TEST_ROOT/machine0a" "$outA" &
pidA=$!

# 2. Wait until that build is in-flight (its marker has streamed to A).
waitFor "grepQuiet '$marker' '$outA'" 60 \
    || { echo "build A never started streaming" >&2; cat "$outA" >&2; exit 1; }

# 3. Second, concurrent build of the *same* derivation, separate local store.
buildOne "$TEST_ROOT/machine0b" "$outB" &
pidB=$!

# 4. The late joiner attaches and the coordinator *replays* the buffered log to
#    it: wait for the marker (emitted before B attached) to surface in B's
#    output. This is the replay/attach proof.
waitFor "grepQuiet '$marker' '$outB'" 60 \
    || { echo "late joiner never saw the replayed marker" >&2; cat "$outB" >&2; exit 1; }

# 5. Both builds finish (the shared build completes after `seconds`).
wait "$pidA"; rA=$?
wait "$pidB"; rB=$?
[ "$rA" -eq 0 ] || { echo "build A failed" >&2; cat "$outA" >&2; exit 1; }
[ "$rB" -eq 0 ] || { echo "build B failed" >&2; cat "$outB" >&2; exit 1; }

# 6. Exactly one real build ran: both clients observed the SAME builder-process
#    token. (Different tokens ⇒ two builds ⇒ no dedup.)
tokA="$(sed -n 's/.*BUILDTOKEN:\([0-9]*\).*/\1/p' "$outA" | head -n1)"
tokB="$(sed -n 's/.*BUILDTOKEN:\([0-9]*\).*/\1/p' "$outB" | head -n1)"
echo "token A=$tokA  token B=$tokB" >&2
[ -n "$tokA" ] || { echo "no build token in A" >&2; cat "$outA" >&2; exit 1; }
[ "$tokA" = "$tokB" ] || { echo "tokens differ ⇒ two builds ran (no dedup)" >&2; exit 1; }

echo "dedup + replay + fan-out OK: one build (token $tokA), two clients, late joiner replayed" >&2
