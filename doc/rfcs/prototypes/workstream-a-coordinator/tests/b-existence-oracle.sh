#!/usr/bin/env bash
# T1 — existence oracle (decisions B1 §4 / spike §3.7). An unauthorized START for
# a resolved key that IS in flight, and for one that is NOT, must return a
# byte-identical denial with no measurable timing difference: authorize() runs
# BEFORE the registry lookup, so the caller learns neither log, drv name, nor
# existence.
. "$(dirname "$0")/lib.sh"
echo "[B-existence-oracle / T1] denial is identical & constant-time across existence"
setup; trap teardown EXIT

OWNER=4001
ATTACKER=4099
INFLIGHT="ca:present-$$;allow=$OWNER"     # this build will be running
ABSENT="ca:absent-$$;allow=$OWNER"        # this one never exists
COUNTER="$SD/counter"

# Bring up a long-running, owner-authorized build (also spawns the coordinator).
cl -k "$INFLIGHT" -d "$INFLIGHT" -u $OWNER -t 0 -c "$COUNTER" -n 40 -s 200 \
   >"$SD/owner.out" 2>"$SD/owner.err" & OWN=$!
sleep 0.6   # let it get in flight + the coordinator come up

# The attacker probes both keys directly on the control socket.
N=15
sum_present=0; sum_absent=0
for i in $(seq 1 $N); do
    ctl --start --drv "$INFLIGHT" --uid $ATTACKER --untrusted >/dev/null 2>"$SD/p.$i"
    ctl --start --drv "$ABSENT"   --uid $ATTACKER --untrusted >/dev/null 2>"$SD/a.$i"
done

# Both must be denied, and the denial text must be identical (no key/existence leak).
assert_contains "in-flight-key probe denied" "$SD/p.1" "START denied"
assert_contains "absent-key probe denied"    "$SD/a.1" "START denied"
P=$(sed 's/ elapsed_us=.*//' "$SD/p.1"); A=$(sed 's/ elapsed_us=.*//' "$SD/a.1")
assert_eq "denial bytes identical regardless of existence" "$A" "$P"

# Timing: medians should be within an order of magnitude (authorize is O(1) and
# independent of the registry). We compare medians with a generous tolerance to
# stay robust on a noisy CI box -- the point is "no existence-dependent branch".
med() { for i in $(seq 1 $N); do sed -n 's/.*elapsed_us=\([0-9]*\).*/\1/p' "$SD/$1.$i"; done | sort -n | awk '{v[NR]=$1} END{print v[int(NR/2)+1]}'; }
MP=$(med p); MA=$(med a)
echo "  MEASURE median_us in_flight=$MP absent=$MA"
# tolerance: difference under 500us (both are sub-millisecond pre-registry denials)
DIFF=$(( MP > MA ? MP - MA : MA - MP ))
assert_lt "no measurable timing difference (|Δmedian| us)" "$DIFF" 500

kill $OWN 2>/dev/null
finish
