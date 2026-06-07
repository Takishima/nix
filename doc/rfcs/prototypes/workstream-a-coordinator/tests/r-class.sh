#!/usr/bin/env bash
# R-class — builder-internal/transient failure classification + no key poisoning.
#
# Inventory test (validation.md "Forward-compatibility & elastic-backend
# additions"; RFC §4.4 / §4.7.5). Gates NO freeze — the failure-classification
# fields ride the *deferred* serve set (decisions Blocker 3), not the frozen 2.9
# diagnostic core. Lives in the throw-away prototype because the semantics it
# exercises (transient vs build-intrinsic class, retry sizing, registry caching)
# are Phase-3 coordinator behaviour that has no production code yet.
#
# Asserts, on the stock coordinator path:
#   1. a builder-internal failure (OOM, exit 137) carries a TRANSIENT/retryable
#      class plus a resource hint ("memory");
#   2. that class is DISTINCT from the BUILD-ERROR class a build-intrinsic
#      failure (exit 1) carries;
#   3. a transient failure is NOT cached/reused — re-running the same key starts
#      a FRESH build (the registry entry was dropped), i.e. no key poisoning.
. "$(dirname "$0")/lib.sh"
echo "[R-class] transient-failure classification + no key poisoning (RFC §4.4)"
setup; trap teardown EXIT
C="$SD/counter"

# --- 1. builder-internal/transient failure (OOM, exit 137) --------------------
KT=rt-$$
cl -k "$KT" -t 1 -c "$C-t" -n 10 -s 120 -X 3 -C 137 >"$SD/t.out" 2>"$SD/t.err"
assert_contains "transient failure reported as a failure"        "$SD/t.err" "status=failure"
assert_contains "builder-internal (OOM) -> class=transient"      "$SD/t.err" "class=transient"
assert_contains "transient carries a resource hint (memory)"     "$SD/t.err" "hint=memory"
assert_eq       "exactly one build ran for the transient attempt" 1 "$(cat "$C-t" 2>/dev/null)"

# --- 2. build-intrinsic failure (exit 1) is a DISTINCT class ------------------
KB=rb-$$
cl -k "$KB" -t 1 -c "$C-b" -n 10 -s 120 -X 3 >"$SD/b.out" 2>"$SD/b.err"   # default exit 1
assert_contains "build-intrinsic failure -> class=build-error"   "$SD/b.err" "class=build-error"
# distinctness: the build-error result must NOT be tagged transient / memory
if grep -q "class=transient" "$SD/b.err"; then bad "build-error is distinct from transient"; \
  else ok "build-error is distinct from transient"; fi
if grep -q "hint=memory" "$SD/b.err"; then bad "build-error carries no resource hint"; \
  else ok "build-error carries no resource hint"; fi

# --- 3. no key poisoning: a transient failure is not cached -------------------
# Re-run the SAME key, this time succeeding. If the failed result had been cached
# under the key, the second run would replay it (counter stays 1). Because the
# registry entry is dropped on finalize, the second run is a FRESH build -> the
# shared counter advances to 2.
cl -k "$KT" -t 1 -c "$C-t" -n 4 -s 80 >"$SD/t2.out" 2>"$SD/t2.err"
assert_contains "re-run of a transiently-failed key succeeds"    "$SD/t2.err" "RESULT ok=1"
assert_contains "re-run is NOT deduplicated against the failure" "$SD/t2.err" "DEDUP=0"
assert_eq       "transient failure not cached -> fresh rebuild (no key poisoning)" \
                2 "$(cat "$C-t" 2>/dev/null)"
finish
