#!/usr/bin/env bash
# M-arch — same package, different system -> two distinct keys, never coalesced.
#
# Inventory test (validation.md "Forward-compatibility & elastic-backend
# additions"; RFC §4.7 multi-architecture pools; §8.1 guardrail #1 "key on the
# resolved drv"). Gates NO freeze. Lives in the throw-away prototype because the
# property is about the *build-key definition* the coordinator enforces, which
# is Phase-3 internal behaviour with no production code yet.
#
# The build key is derived from the (system-specific) derivation, so the same
# package built for x86_64-linux and for aarch64-linux are DIFFERENT keys. The
# coordinator must therefore run two independent builds and never dedup one
# against the other — even when both hit the same endpoint concurrently. Contrast
# A-dedup, where the SAME key collapses to a single build.
. "$(dirname "$0")/lib.sh"
echo "[M-arch] same package, different system -> two distinct keys (RFC §4.7)"
setup; trap teardown EXIT

# Same package "hello", two systems -> two distinct derivations -> two keys.
PKG=hello-$$
K_X86="$PKG.x86_64-linux"
K_ARM="$PKG.aarch64-linux"
# Deliberately SHARE one counter file so "did both builds run?" is a single
# number: coalesced -> 1, correctly-distinct -> 2.
COUNTER="$SD/march-counter"

cl -k "$K_X86" -t 1 -c "$COUNTER" -n 8 -s 200 >"$SD/x86.out" 2>"$SD/x86.err" & X=$!
sleep 0.4   # x86 build is now in flight; an erroneous coalescer would attach here
cl -k "$K_ARM" -t 1 -c "$COUNTER" -n 8 -s 200 >"$SD/arm.out" 2>"$SD/arm.err" & A=$!
wait $X $A

assert_contains "x86_64 build succeeded"                 "$SD/x86.err" "RESULT ok=1"
assert_contains "aarch64 build succeeded"                 "$SD/arm.err" "RESULT ok=1"
assert_contains "x86_64 ran its own build (not deduped)"  "$SD/x86.err" "DEDUP=0"
assert_contains "aarch64 ran its own build (not deduped)" "$SD/arm.err" "DEDUP=0"
assert_eq       "two distinct keys -> two builds, never coalesced" 2 "$(cat "$COUNTER" 2>/dev/null)"

# Cross-check that each result is scoped to its own system: the logs name the
# system-specific key, so a client never sees the other architecture's build.
assert_contains "x86_64 client saw only its own system's log"  "$SD/x86.out" "$K_X86"
assert_contains "aarch64 client saw only its own system's log" "$SD/arm.out" "$K_ARM"
if grep -q "$K_ARM" "$SD/x86.out"; then bad "x86_64 client did not see aarch64 output"; \
  else ok "x86_64 client did not see aarch64 output"; fi

# Positive control: the SAME key DOES coalesce (this is the A-dedup property),
# proving the distinctness above is about the key, not an artefact of timing.
KS="$PKG.same"
SC="$SD/same-counter"
cl -k "$KS" -t 1 -c "$SC" -n 8 -s 200 >"$SD/s1.out" 2>"$SD/s1.err" & S1=$!
sleep 0.4
cl -k "$KS" -t 1 -c "$SC" -n 8 -s 200 >"$SD/s2.out" 2>"$SD/s2.err" & S2=$!
wait $S1 $S2
assert_eq "control: identical key DOES coalesce to one build" 1 "$(cat "$SC" 2>/dev/null)"
finish
