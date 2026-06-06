#!/usr/bin/env bash
# T3 — asserted-key spoof (decisions B1 §4 / spike §3.2). A child that presents a
# buildKey not matching the drv it sends is rejected: the coordinator recomputes
# the canonical key from the drv material it received and never trusts the
# asserted key. The rejection is independent of authorization (the spoofer here
# IS authorized for the drv).
. "$(dirname "$0")/lib.sh"
echo "[B-spoof / T3] coordinator recomputes the key; asserted-key mismatch rejected"
setup; trap teardown EXIT

UID4=4001
DRV="ca:realkey-$$;allow=$UID4"
COUNTER="$SD/counter"

# Bring the coordinator up with a real build so the control socket exists.
cl -k "$DRV" -d "$DRV" -u $UID4 -t 0 -c "$COUNTER" -n 20 -s 200 >"$SD/r.out" 2>"$SD/r.err" & R=$!
sleep 0.5

# Honest probe: asserted key == drv -> accepted (authorized + key matches).
ctl --start --drv "$DRV" --key "$DRV" --uid $UID4 --untrusted >/dev/null 2>"$SD/honest.err"
assert_contains "honest matching-key start is accepted" "$SD/honest.err" "START ok"

# Spoof probe: same drv (so authorize passes) but a DIFFERENT asserted key.
ctl --start --drv "$DRV" --key "ca:EVIL-$$" --uid $UID4 --untrusted >/dev/null 2>"$SD/spoof.err"
assert_contains "spoofed asserted key is rejected"     "$SD/spoof.err" "START denied"
assert_contains "coordinator logged the key mismatch"  "$SD/daemon.err" "asserted key mismatch"

kill $R 2>/dev/null
finish
