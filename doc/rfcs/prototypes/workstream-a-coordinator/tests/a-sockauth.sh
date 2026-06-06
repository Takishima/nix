#!/usr/bin/env bash
# A-sockauth: a process whose uid is not the daemon's own uid is refused at the
# SO_PEERCRED peer-cred check, BEFORE any sessionAuth is read (spike §3.7.1).
# A-trust: an untrusted caller cannot start/attach an input-addressed build it
# could not itself request, and QUERY_ACTIVE is filtered (spike §3.7.2/§3.7.3).
. "$(dirname "$0")/lib.sh"
echo "[A-sockauth] peer-cred socket auth + authorize-before-subscribe + query filtering"

# Force a uid mismatch: tell the coordinator to expect a different daemon uid, so
# our (same-uid) connections are refused at the peer-cred check.
ME=$(id -u)
export WSA_FAKE_DAEMON_UID=$(( ME + 1 ))
setup; trap teardown EXIT

# Touch the coordinator so the daemon spawns it. The relay child connects to the
# coordinator as our uid; the coordinator (expecting uid ME+1) refuses it, and
# the child falls back to a local build -- the client still succeeds, but no
# coordinator session was ever established.
cl -k sa-$$ -t 1 -c "$SD/counter" -n 2 -s 50 >"$SD/c.out" 2>"$SD/c.err"
assert_contains "control connection from wrong uid was REFUSED" "$SD/daemon.err" "REFUSED control connection"

# A direct control-socket probe as the wrong uid sees an immediate EOF.
ctl >"$SD/ctl.out" 2>"$SD/ctl.err" || true
assert_contains "direct probe with wrong uid is refused" "$SD/ctl.err" "REFUSED"

# Now run with correct uid to exercise authorize() + QUERY_ACTIVE filtering.
unset WSA_FAKE_DAEMON_UID
teardown; setup; trap teardown EXIT

# Untrusted client requesting an input-addressed key it cannot build -> DENIED.
cl -k "ia-secret-$$" -t 0 -c "$SD/counter2" -n 2 -s 50 >"$SD/u.out" 2>"$SD/u.err"
assert_contains "untrusted start/attach of a non-CA build is denied" "$SD/u.err" "DENIED"

# Start a trusted input-addressed build, then query as untrusted: must not see it.
cl -k "ia-private-$$" -t 1 -c "$SD/counter3" -n 12 -s 200 >"$SD/t.out" 2>"$SD/t.err" & T=$!
sleep 0.5
ctl --untrusted >"$SD/q.out" 2>"$SD/q.err" || true
assert_contains "untrusted QUERY_ACTIVE sees zero builds"   "$SD/q.err" "ACTIVE count=0"
ctl >"$SD/q2.out" 2>"$SD/q2.err" || true
assert_contains "trusted QUERY_ACTIVE sees the in-flight build" "$SD/q2.err" "ia-private-$$"
wait $T 2>/dev/null
finish
