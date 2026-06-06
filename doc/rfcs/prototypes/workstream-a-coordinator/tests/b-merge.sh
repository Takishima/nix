#!/usr/bin/env bash
# T2 — CA-merge re-authorization (decisions B1 §4 / spike §3.8). Two *distinct*
# unresolved drvs that resolve to the SAME resolved key: caller A is authorized
# for the resolved key, caller B is not. At promotion the coordinator
# re-authorizes every subscriber against the resolved key; B is detached with an
# error and observes NONE of A's build log; A's build runs exactly once.
. "$(dirname "$0")/lib.sh"
echo "[B-merge / T2] re-authorize on CA promotion; unauthorized caller detached"
setup; trap teardown EXIT

UA=4001            # caller A's uid (authorized for the resolved key)
UB=4002            # caller B's uid (NOT in the resolved key's allowlist)
R="ca:merged-$$;allow=$UA"        # resolved key authorizes only A
COUNTER="$SD/counter"

# A resolves first (300ms) -> becomes the running resolved build.
cl --ca -U "ca:uA-$$;allow=$UA" -V "$R" -M 300 -u $UA -t 0 -c "$COUNTER" -n 8 -s 150 \
   >"$SD/a.out" 2>"$SD/a.err" & A=$!
# B resolves later (650ms) to the SAME resolved key, but is not authorized for it.
cl --ca -U "ca:uB-$$;allow=$UB" -V "$R" -M 650 -u $UB -t 0 -c "$COUNTER" -n 8 -s 150 \
   >"$SD/b.out" 2>"$SD/b.err" & B=$!
wait $A $B

assert_contains "A (authorized) completed the build"        "$SD/a.err" "RESULT ok=1"
assert_contains "B (unauthorized) was rejected with error"  "$SD/b.err" "RESULT ok=0"
assert_contains "B was detached at re-authorization"        "$SD/daemon.err" "RE-AUTH FAILED"
assert_eq       "exactly one build ran (merged on the resolved key)" 1 "$(cat "$COUNTER" 2>/dev/null)"
# The critical security property: B never saw A's build log.
if grep -q "marker line" "$SD/b.out" 2>/dev/null; then
    bad "B leaked A's build log"
else
    ok "B observed none of A's build log"
fi
finish
