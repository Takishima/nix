#!/usr/bin/env bash

# Elastic builder opt-in: a builder flagged `self-scheduled` in /etc/nix/machines
# (column 9) makes the distributed-build hook treat `maxJobs` as a load-balancing
# hint, not a hard cap — so it never postpones a build for want of a local slot.
#
# Proof: one builder with maxJobs=1 and the elastic flag set. Two *different*
# derivations are offloaded to it concurrently via the build hook (--max-jobs 0).
# With the default hard-cap semantics maxJobs=1 would serialise them; with the
# elastic flag both run at once. The test asserts that when the second build has
# started on the builder, the first is *still* running — overlap on a one-slot
# builder is only possible if maxJobs was treated as a hint.
#
# fake-SSH to localhost (no sshd): `ssh-ng://localhost` runs the remote daemon
# as a local subprocess. The builds stay in-flight for `iters` so the overlap is
# observable.

source common.sh

TODO_NixOS

[[ "${busybox-}" =~ busybox ]] || skipTest "no busybox"

unset NIX_STORE_DIR

chmod -R +w "$TEST_ROOT/machine"* 2>/dev/null || true
rm -rf "$TEST_ROOT/machine"* || true

# ~444K iters/s in busybox ash on this CI, so ~9M ≈ 20s — long enough that the
# first build is still running when the second one starts.
iters=9000000

# maxJobs=1, and the elastic flag in column 9 (URI systems key maxJobs speed
# supported mandatory hostkey ELASTIC).
builder="ssh-ng://localhost?remote-store=$TEST_ROOT/machine1 - - 1 1 - - - true"

buildOne() { # $1 = local store, $2 = marker, $3 = outfile
    nix build -L -f build-dedup-coordinator.nix slow \
        --arg busybox "$busybox" \
        --argstr marker "$2" --argstr iters "$iters" \
        --no-link --max-jobs 0 \
        --store "$1" \
        --builders "$builder" > "$3" 2>&1
}

waitFor() { # $1 = predicate, $2 = secs
    local i=0
    while ! eval "$1"; do
        sleep 0.2
        i=$((i + 1))
        [ "$i" -lt "$(( $2 * 5 ))" ] || return 1
    done
}

mA="elastic-A-$$"
mB="elastic-B-$$"
outA="$TEST_ROOT/elastic-A.out"
outB="$TEST_ROOT/elastic-B.out"

# 1. First build: start it and let it hold the single slot.
buildOne "$TEST_ROOT/machine0a" "$mA" "$outA" &
pidA=$!
waitFor "grepQuiet '$mA' '$outA'" 60 \
    || { echo "build A never started streaming" >&2; cat "$outA" >&2; exit 1; }

# 2. Second, *different* build, concurrently, to the same one-slot builder.
buildOne "$TEST_ROOT/machine0b" "$mB" "$outB" &
pidB=$!
waitFor "grepQuiet '$mB' '$outB'" 60 \
    || { echo "build B never started streaming" >&2; cat "$outB" >&2; exit 1; }

# 3. B started while A is still running ⇒ both ran at once on a maxJobs=1
#    builder ⇒ the elastic flag turned maxJobs into a hint. Were it a hard cap,
#    B would only start after A finished, by which point A's client has exited.
kill -0 "$pidA" 2>/dev/null \
    || { echo "build A had finished before B started ⇒ serialized (elastic flag ignored)" >&2; exit 1; }

# 4. Both builds succeed.
wait "$pidA"; rA=$?
wait "$pidB"; rB=$?
[ "$rA" -eq 0 ] || { echo "build A failed" >&2; cat "$outA" >&2; exit 1; }
[ "$rB" -eq 0 ] || { echo "build B failed" >&2; cat "$outB" >&2; exit 1; }

echo "elastic OK: two builds ran concurrently on a maxJobs=1 self-scheduled builder" >&2
