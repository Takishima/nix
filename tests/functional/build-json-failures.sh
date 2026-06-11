#!/usr/bin/env bash

# The machine-readable failure surface: `nix build --json` emits a
# per-derivation entry for every *completed* build, failures included, so a
# scheduler / CI driver can read the structured failure facts (status,
# exitCode, logTail, logRef, failureClass, killedForMemory,
# peakMemoryBytes, ...) from stdout instead of scraping stderr. Stdout was
# previously empty whenever the build failed.

source common.sh

clearStoreIfPossible

out="$TEST_ROOT/build-json-failures.json"
stderrFile="$TEST_ROOT/build-json-failures.stderr"

# --- mixed success/failure under --keep-going -------------------------------

nix build -f build-json-failures.nix good bad1 bad2 \
    --json --keep-going --no-link > "$out" 2> "$stderrFile" && status=0 || status=$?

# The failure must still fail the command, with the same exit status and the
# same human-readable stderr rendering as without --json.
[[ "$status" != 0 ]]
nix build -f build-json-failures.nix good bad1 bad2 \
    --keep-going --no-link 2> /dev/null && status2=0 || status2=$?
[[ "$status" = "$status2" ]]
grepQuiet "json-failures-bad1-marker line 2" "$stderrFile"

# stdout is one JSON array with an entry per derivation.
jq --exit-status 'length == 3' < "$out"
jq --exit-status '[.[] | select(.success == false)] | length == 2' < "$out"

# The success entry keeps the pre-existing shape (drvPath + outputs, and no
# "success" discriminator), so existing consumers are unaffected.
jq --exit-status '
  [.[] | select(.success == null)] |
  length == 1 and
  (.[0] |
    (has("success") | not) and
    (.drvPath | match(".*json-failures-good.drv")) and
    (.outputs.out | match(".*json-failures-good")))
' < "$out"

# Each failure entry names the derivation it is about and carries the
# structured failure facts.
jq --exit-status '
  [.[] | select(.success == false)] | all(
    (.drvPath | match(".*json-failures-bad[12].drv")) and
    (.status == "PermanentFailure" or .status == "MiscFailure") and
    (.errorMsg | length > 0) and
    (.exitCode != 0)
  )
' < "$out"

# `exitCode` is the builder's real exit code, decoded from the raw wait(2)
# status — `exit 3` → 3 and `exit 4` → 4, not 768 / 1024 (the undecoded
# `status << 8`).
jq --exit-status '
  ([.[] | select(.drvPath | match(".*-bad1.drv"))][0].exitCode == 3) and
  ([.[] | select(.drvPath | match(".*-bad2.drv"))][0].exitCode == 4)
' < "$out"

# The diagnostic core rides along: the log tail (so a failure can be
# post-processed with no re-run) and the key under which `nix log` can fetch
# the full log.
jq --exit-status '
  [.[] | select(.success == false) | select(.drvPath | match(".*-bad1.drv"))] |
  .[0] |
  (.logTail | match("json-failures-bad1-marker line 2")) and
  (.logRef | match(".*json-failures-bad1.drv"))
' < "$out"

# An ordinary local failure is build-intrinsic: the retry-classification
# fields stay absent (absent means failureClass == "BuildError", not killed
# for memory, peak memory unmeasured).
jq --exit-status '
  [.[] | select(.success == false)] | all(
    (has("failureClass") | not) and (has("killedForMemory") | not)
  )
' < "$out"

# --- a single failure, without --keep-going ---------------------------------

nix build -f build-json-failures.nix bad1 \
    --json --no-link > "$out" 2> /dev/null && status=0 || status=$?
[[ "$status" != 0 ]]
jq --exit-status '
  length == 1 and
  (.[0] |
    .success == false and
    (.drvPath | match(".*json-failures-bad1.drv")) and
    (.logTail | match("json-failures-bad1-marker line 1")))
' < "$out"

# --- an all-success run is byte-compatible with the old output --------------

nix build -f build-json-failures.nix good --json --no-link | jq --exit-status '
  length == 1 and
  (.[0] |
    (has("success") | not) and
    (has("errorMsg") | not) and
    (.outputs.out | match(".*json-failures-good")))
'
