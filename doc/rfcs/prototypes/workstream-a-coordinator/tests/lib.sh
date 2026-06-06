# Workstream A prototype — shared test helpers.  THROW-AWAY CODE.
# Sourced by every tests/a-*.sh. Provides setup/teardown of a private state dir +
# relay daemon, and tiny assertion helpers that print a one-line PASS/FAIL.

set -u

BINDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PASS=0
FAIL=0

setup() {
    SD="$(mktemp -d /tmp/wsa-test.XXXXXX)"
    export WSA_STATE_DIR="$SD"
    export WSA_DEBUG=1
    # capture daemon + (spawned) coordinator stderr together for assertions
    "$BINDIR/reldaemon" "$SD" >"$SD/daemon.out" 2>"$SD/daemon.err" &
    DAEMON_PID=$!
    # wait for the daemon socket
    for _ in $(seq 1 200); do [ -S "$SD/daemon.socket" ] && break; sleep 0.01; done
}

teardown() {
    [ -n "${DAEMON_PID:-}" ] && kill "$DAEMON_PID" 2>/dev/null
    # kill any coordinator the daemon spawned
    pkill -f "$BINDIR/coordinator $SD" 2>/dev/null || true
    [ -n "${SD:-}" ] && rm -rf "$SD"
}

cl()   { "$BINDIR/client" --state "$SD" "$@"; }   # run a client
ctl()  { "$BINDIR/ctl"    --state "$SD" "$@"; }   # probe the control socket

ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }

assert_eq() { # desc expected actual
    if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1 (expected '$2', got '$3')"; fi
}
assert_gt() { # desc actual min
    if [ "$2" -gt "$3" ] 2>/dev/null; then ok "$1 ($2 > $3)"; else bad "$1 (got '$2', want > $3)"; fi
}
assert_lt() { # desc actual max
    if [ "$2" -lt "$3" ] 2>/dev/null; then ok "$1 ($2 < $3)"; else bad "$1 (got '$2', want < $3)"; fi
}
assert_contains() { # desc file pattern
    if grep -q -- "$3" "$2" 2>/dev/null; then ok "$1"; else bad "$1 (pattern '$3' not in $2)"; fi
}
assert_files_equal() { # desc a b
    if diff -q "$2" "$3" >/dev/null 2>&1; then ok "$1"; else bad "$1 (files differ)"; fi
}

finish() {
    echo "  -- $PASS passed, $FAIL failed --"
    [ "$FAIL" -eq 0 ]
}
