#!/usr/bin/env bash
# Battle-test matrix for the remote-build-protocol branch.
# Runs from the host against the kind (or hetzner) cluster via kubectl exec.
# Prints a PASS/FAIL table; artifacts land in .state/results-<RUN>/.
#
# Usage: ./run-tests.sh [testnum...]   (default: all)
set -uo pipefail
cd "$(dirname "$0")"

NS=battletest
RUN=${RUN:-$(date +%H%M%S)}
ART=.state/results-$RUN
mkdir -p "$ART"

declare -a RESULTS=()

log()    { echo "[$(date +%T)] $*"; }
record() { RESULTS+=("$1|$2|$3"); log "== $1: $2 ($3)"; }

# kc <args...>: kubectl in our namespace
kc() { kubectl -n $NS "$@"; }

# cx <pod> <script> [timeout]: run a bash script in a pod, with RUN exported.
cx() {
    local pod=$1 script=$2 t=${3:-240}
    timeout "$t" kubectl -n $NS exec "$pod" -- env "RUN=$RUN" bash -c "$script"
}

# Machine-spec one-liners for --builders overrides (column 9 = isElastic).
M0='ssh://builder-0.builders x86_64-linux /root/.ssh/id_ed25519 4 1'
M1='ssh://builder-1.builders x86_64-linux /root/.ssh/id_ed25519 4 1'
M1NG='ssh-ng://builder-1.builders x86_64-linux /root/.ssh/id_ed25519 4 1'
MOOM='ssh://builder-oom-0.builders x86_64-linux /root/.ssh/id_ed25519 4 1'
MSTOCK='ssh://builder-stock-0.builders x86_64-linux /root/.ssh/id_ed25519 4 1'

# Count live build processes on a builder pod whose cmdline contains a marker.
# ([x] bracket trick so the probe itself never matches.)
count_builds() {
    local pod=$1 marker=$2
    cx "$pod" '
        c=0
        for f in /proc/[0-9]*/cmdline; do
            grep -qa "'"$marker"'" "$f" 2>/dev/null && c=$((c+1))
        done
        echo $c' 30
}

# ---------------------------------------------------------------------------
test_1() { # ssh:// build via hook; fail-loud on failure
    local name="1-hook-ssh-and-fail-loud"
    if ! cx client-0 'nix-build /tests/exprs.nix -A quick --argstr salt "$RUN-t1" --no-out-link' \
            > "$ART/t1-ok.log" 2>&1; then
        record "$name" FAIL "plain hook build failed (see t1-ok.log)"; return
    fi
    cx client-0 'nix-build /tests/exprs.nix -A fail --argstr salt "$RUN-t1f" --no-out-link' \
        > "$ART/t1-fail.log" 2>&1
    local rc=$?
    if [ $rc -eq 0 ]; then
        record "$name" FAIL "failing drv unexpectedly succeeded"; return
    fi
    if grep -q "bt-fail-last-words" "$ART/t1-fail.log"; then
        record "$name" PASS "build ok; failure shows remote log tail inline"
    else
        record "$name" FAIL "no remote log tail in client error (see t1-fail.log)"
    fi
}

# ---------------------------------------------------------------------------
stream_one() { # helper: build `slow` with a builders override, assert lines appear live
    local builders=$1 tag=$2
    cx client-0 '
        rm -f /tmp/st.log
        nohup nix build -f /tests/exprs.nix slow --argstr salt "$RUN-'"$tag"'" \
            --builders "'"$builders"'" -L --no-link > /tmp/st.log 2>&1 &
        live=no
        for i in $(seq 1 90); do
            if grep -q bt-slow-done /tmp/st.log; then break; fi
            if grep -q bt-slow-line-3 /tmp/st.log; then live=yes; break; fi
            sleep 0.5
        done
        # wait for the build to finish either way
        for i in $(seq 1 120); do
            grep -q bt-slow-done /tmp/st.log && break
            grep -qiE "error" /tmp/st.log && break
            sleep 1
        done
        cp /tmp/st.log /tmp/st-keep.log
        echo "live=$live"' 200
}

test_2() { # live streaming over ssh:// and ssh-ng://
    local name="2-live-streaming"
    local r1 r2
    r1=$(stream_one "$M0" t2a) ; cx client-0 'cat /tmp/st-keep.log' 30 > "$ART/t2-ssh.log" 2>&1
    r2=$(stream_one "$M1NG" t2b); cx client-0 'cat /tmp/st-keep.log' 30 > "$ART/t2-sshng.log" 2>&1
    local ok1=no ok2=no
    [[ $r1 == *live=yes* ]] && ok1=yes
    [[ $r2 == *live=yes* ]] && ok2=yes
    if [ "$ok1$ok2" = yesyes ]; then
        record "$name" PASS "log lines visible mid-build on ssh:// and ssh-ng://"
    else
        record "$name" FAIL "ssh=$ok1 ssh-ng=$ok2 (see t2-*.log)"
    fi
}

# ---------------------------------------------------------------------------
test_3() { # nix log --store ssh:// after the fact
    local name="3-nix-log-over-ssh"
    local out
    out=$(cx client-0 '
        drv=$(nix-instantiate /tests/exprs.nix -A quick --argstr salt "$RUN-t3" 2>/dev/null)
        nix-build /tests/exprs.nix -A quick --argstr salt "$RUN-t3" --no-out-link \
            --builders "'"$M0"'" >/dev/null 2>&1 || { echo BUILD-FAILED; exit 1; }
        nix log --store ssh://builder-0.builders "$drv"' 180)
    echo "$out" > "$ART/t3.log"
    if echo "$out" | grep -q "bt-quick-token:"; then
        record "$name" PASS "real build log returned from builder over ssh://"
    else
        record "$name" FAIL "log content missing (see t3.log)"
    fi
}

# ---------------------------------------------------------------------------
test_4() { # --eval-store split + no-op rerun + nix store cat
    local name="4-eval-store-split"
    local out
    out=$(cx client-0 '
        set -e
        rm -rf /tmp/bt-eval
        outp=$(nix-build --eval-store /tmp/bt-eval --store ssh://builder-0.builders \
                 /tests/exprs.nix -A quickFile --argstr salt "$RUN-t4" --no-out-link 2>/tmp/t4-first.err)
        echo "OUT=$outp"
        nix-build --eval-store /tmp/bt-eval --store ssh://builder-0.builders \
            /tests/exprs.nix -A quickFile --argstr salt "$RUN-t4" --no-out-link >/dev/null 2>/tmp/t4-second.err
        if grep -qE "will be built|building " /tmp/t4-second.err; then echo REBUILD=yes; else echo REBUILD=no; fi
        echo "CAT=$(nix store cat --store ssh://builder-0.builders "$outp/payload.txt")"
        cat /tmp/t4-first.err /tmp/t4-second.err >&2' 240 2>"$ART/t4.err")
    echo "$out" > "$ART/t4.log"
    if echo "$out" | grep -q "REBUILD=no" && echo "$out" | grep -q "CAT=payload-$RUN-t4"; then
        record "$name" PASS "build, no-op re-run, and remote store cat all work"
    else
        record "$name" FAIL "out: $(echo "$out" | tr '\n' ' ') (see t4.log)"
    fi
}

# ---------------------------------------------------------------------------
# Start a detached build of `slower` against ssh-ng://builder-0 in a client pod.
# Leaves /tmp/bt-dedup.log, /tmp/bt-dedup.pid, /tmp/bt-dedup.rc in the pod.
start_dedup_build() {
    local pod=$1 salt=$2
    cx "$pod" '
        rm -f /tmp/bt-dedup.log /tmp/bt-dedup.rc
        nohup bash -c "nix build -f /tests/exprs.nix slower --argstr salt \"'"$salt"'\" \
            --store ssh-ng://builder-0.builders --eval-store auto -L --no-link \
            > /tmp/bt-dedup.log 2>&1; echo \$? > /tmp/bt-dedup.rc" >/dev/null 2>&1 &
        echo $! > /tmp/bt-dedup.pid
        echo started' 30
}

# Kill the client's nix build and any orphaned ssh transport to the builder,
# simulating client death.
kill_dedup_build() {
    local pod=$1
    cx "$pod" '
        pid=$(cat /tmp/bt-dedup.pid 2>/dev/null) || exit 0
        # kill the whole subtree under the nohup bash
        for p in /proc/[0-9]*; do
            [ "$(cat $p/stat 2>/dev/null | awk "{print \$4}")" = "$pid" ] && kill -9 "${p#/proc/}" 2>/dev/null
        done
        kill -9 "$pid" 2>/dev/null
        sleep 0.5
        # sweep nix/ssh leftovers of this build
        for f in /proc/[0-9]*/cmdline; do
            if grep -qa "builder-0.builder[s]" "$f" 2>/dev/null; then
                kill -9 "$(echo "$f" | cut -d/ -f3)" 2>/dev/null
            fi
        done
        echo killed' 30
}

test_5() { # dedup + active-builds + refcounted cancellation
    local name="5-dedup"
    local salt="$RUN-t5"
    start_dedup_build client-0 "$salt" >/dev/null
    sleep 3
    start_dedup_build client-1 "$salt" >/dev/null
    sleep 10

    local active subs=0
    for i in $(seq 1 15); do
        active=$(cx builder-0 'nix store active-builds --json 2>/dev/null || nix store active-builds' 30)
        subs=$(echo "$active" | grep -oE '"subscribers": ?[0-9]+' | grep -oE '[0-9]+$' | sort -rn | head -1)
        [ "${subs:-0}" -ge 2 ] && break
        sleep 2
    done
    echo "$active" > "$ART/t5-active-builds.json"

    local nbuilds
    nbuilds=$(count_builds builder-0 'bt-slower-star[t]')

    # wait for both clients to finish
    local rc0 rc1 tok0 tok1
    cx client-0 'for i in $(seq 1 120); do [ -f /tmp/bt-dedup.rc ] && break; sleep 1; done' 150 >/dev/null
    cx client-1 'for i in $(seq 1 120); do [ -f /tmp/bt-dedup.rc ] && break; sleep 1; done' 150 >/dev/null
    rc0=$(cx client-0 'cat /tmp/bt-dedup.rc 2>/dev/null' 20)
    rc1=$(cx client-1 'cat /tmp/bt-dedup.rc 2>/dev/null' 20)
    tok0=$(cx client-0 'grep -o "bt-slower-start token:[0-9]*" /tmp/bt-dedup.log | head -1' 20)
    tok1=$(cx client-1 'grep -o "bt-slower-start token:[0-9]*" /tmp/bt-dedup.log | head -1' 20)
    cx client-0 'cat /tmp/bt-dedup.log' 20 > "$ART/t5-client-0.log" 2>&1
    cx client-1 'cat /tmp/bt-dedup.log' 20 > "$ART/t5-client-1.log" 2>&1

    local detail="rc=$rc0/$rc1 tokens=$tok0/$tok1 concurrent-builds=$nbuilds max-subscribers=${subs:-?}"
    if [ "$rc0" = 0 ] && [ "$rc1" = 0 ] && [ -n "$tok0" ] && [ "$tok0" = "$tok1" ] \
        && [ "$nbuilds" = 1 ] && [ "${subs:-0}" -ge 2 ]; then
        record "$name" PASS "$detail"
    else
        record "$name" FAIL "$detail"
    fi

    # --- refcounted cancellation ---
    name="5b-refcount-cancel"
    local saltc="$RUN-t5c"
    start_dedup_build client-0 "$saltc" >/dev/null
    sleep 3
    start_dedup_build client-1 "$saltc" >/dev/null
    sleep 8
    local before
    before=$(count_builds builder-0 'bt-slower-star[t]')
    kill_dedup_build client-0 >/dev/null
    sleep 6
    local after1
    after1=$(count_builds builder-0 'bt-slower-star[t]')
    cx builder-0 'nix store active-builds --json 2>/dev/null' 30 > "$ART/t5c-active-after-kill1.json"
    kill_dedup_build client-1 >/dev/null
    local after2=999
    for i in $(seq 1 15); do
        sleep 2
        after2=$(count_builds builder-0 'bt-slower-star[t]')
        [ "$after2" = 0 ] && break
    done
    detail="builds before=$before after-kill-one=$after1 after-kill-both=$after2"
    if [ "$before" = 1 ] && [ "$after1" = 1 ] && [ "$after2" = 0 ]; then
        record "$name" PASS "$detail"
    else
        record "$name" FAIL "$detail"
    fi
}

# ---------------------------------------------------------------------------
# Wait until client-0 has no nix-build of our exprs running any more.
wait_client_idle() {
    for i in $(seq 1 120); do
        local c
        c=$(count_builds client-0 'exprs.ni[x]')
        [ "${c:-0}" = 0 ] && return 0
        sleep 2
    done
    return 1
}

elastic_round() { # helper: two parallel hook builds, returns max concurrency seen
    local builders=$1 s1=$2 s2=$3
    cx client-0 '
        nohup nix-build /tests/exprs.nix -A slow --argstr salt "$RUN-'"$s1"'" --no-out-link \
            --builders "'"$builders"'" > /tmp/e1.log 2>&1 &
        nohup nix-build /tests/exprs.nix -A slow --argstr salt "$RUN-'"$s2"'" --no-out-link \
            --builders "'"$builders"'" > /tmp/e2.log 2>&1 &
        echo started' 30 >/dev/null
    local max=0 c seen=0 zeros=0
    for i in $(seq 1 60); do
        c=$(count_builds builder-0 'bt-slow-star[t]')
        c=${c:-0}
        [ "$c" -gt "$max" ] && max=$c
        if [ "$c" -gt 0 ]; then
            seen=1; zeros=0
        elif [ "$seen" = 1 ]; then
            zeros=$((zeros+1))
            # the hook starts the queued second build within a couple of
            # seconds of a slot freeing; 5 consecutive zero-polls = all done
            [ "$zeros" -ge 5 ] && break
        fi
        sleep 2
    done
    wait_client_idle
    echo "$max"
}

test_6() { # self-scheduled (elastic) builder exceeds maxJobs
    local name="6-elastic-maxjobs"
    # control: maxJobs=1, not elastic -> never more than 1 concurrent
    local mctrl='ssh://builder-0.builders x86_64-linux /root/.ssh/id_ed25519 1 1 - - - -'
    local melast='ssh://builder-0.builders x86_64-linux /root/.ssh/id_ed25519 1 1 - - - true'
    local maxc maxe
    maxc=$(elastic_round "$mctrl" t6c1 t6c2 | tail -1)
    maxe=$(elastic_round "$melast" t6e1 t6e2 | tail -1)
    local detail="max-concurrency control=$maxc elastic=$maxe (maxJobs=1)"
    if [ "${maxc:-9}" -le 1 ] && [ "${maxe:-0}" -ge 2 ]; then
        record "$name" PASS "$detail"
    else
        record "$name" FAIL "$detail"
    fi
}

# ---------------------------------------------------------------------------
test_7() { # use-ssh-ng-for-remote-builds routes schemeless entries over daemon
    local name="7-ssh-ng-default"
    local mplain='builder-1.builders x86_64-linux /root/.ssh/id_ed25519 4 1'
    cx client-0 '
        nohup nix-build /tests/exprs.nix -A slow --argstr salt "$RUN-t7" --no-out-link \
            --option use-ssh-ng-for-remote-builds true \
            --builders "'"$mplain"'" > /tmp/t7.log 2>&1 &
        echo started' 30 >/dev/null
    local sawdaemon=0
    for i in $(seq 1 25); do
        local c
        c=$(count_builds builder-1 'nix-daemo[n] --stdio')
        [ -n "$c" ] && [ "$c" -ge 1 ] && { sawdaemon=1; break; }
        sleep 2
    done
    cx client-0 'for i in $(seq 1 90); do grep -q bt-slow-done /tmp/t7.log 2>/dev/null && break; sleep 1; done; tail -3 /tmp/t7.log' 120 > "$ART/t7.log" 2>&1
    if [ "$sawdaemon" = 1 ]; then
        record "$name" PASS "schemeless machine entry spawned nix-daemon --stdio on builder"
    else
        record "$name" FAIL "no nix-daemon --stdio seen on builder-1 during build"
    fi
}

# ---------------------------------------------------------------------------
test_8() { # OOM-killed remote build
    local name="8-oom-classification"
    cx client-0 'nix build -f /tests/exprs.nix memhog --argstr salt "$RUN-t8" --no-link -L \
        --builders "'"$MOOM"'"' 300 > "$ART/t8.log" 2>&1
    local rc=$?
    if [ $rc -eq 0 ]; then
        record "$name" FAIL "memhog build unexpectedly succeeded"; return
    fi
    if grep -q "bt-memhog-survived" "$ART/t8.log"; then
        record "$name" FAIL "memory limit not enforced in this environment (build survived to cap)"
        return
    fi
    if grep -qiE "SIGKILL|signal 9|137|out-of-memory|out of memory|ResourceExhausted" "$ART/t8.log"; then
        record "$name" PASS "killed build rendered with OOM/SIGKILL context (see t8.log)"
    else
        record "$name" FAIL "build failed but no SIGKILL/OOM rendering (see t8.log)"
    fi
}

# ---------------------------------------------------------------------------
test_9() { # stock upstream builder: graceful degradation
    local name="9-stock-degradation"
    if ! cx client-0 'nix-build /tests/exprs.nix -A quick --argstr salt "$RUN-t9" --no-out-link \
            --builders "'"$MSTOCK"'"' 180 > "$ART/t9-build.log" 2>&1; then
        record "$name" FAIL "plain build against stock builder failed (see t9-build.log)"
        return
    fi
    # nix log over ssh:// must fail cleanly (no hang, no protocol error)
    cx client-0 '
        drv=$(nix-instantiate /tests/exprs.nix -A quick --argstr salt "$RUN-t9" 2>/dev/null)
        nix log --store ssh://builder-stock-0.builders "$drv"' 60 > "$ART/t9-log.log" 2>&1
    local rc=$?
    if [ $rc -eq 124 ]; then
        record "$name" FAIL "nix log against stock builder HUNG (timeout)"
    elif [ $rc -eq 0 ] && grep -q "bt-quick-token" "$ART/t9-log.log"; then
        record "$name" FAIL "nix log unexpectedly returned a log from a 2.8 peer (?)"
    elif grep -qiE "unexpected|corrupt|assertion|unknown command" "$ART/t9-log.log"; then
        record "$name" FAIL "protocol-level error talking to stock builder (see t9-log.log)"
    else
        record "$name" PASS "build works; nix log degrades cleanly (rc=$rc)"
    fi
}

# ---------------------------------------------------------------------------
ALL_TESTS=(1 2 3 4 5 6 7 8 9)
if [ $# -gt 0 ]; then TESTS=("$@"); else TESTS=("${ALL_TESTS[@]}"); fi

log "battletest run id: $RUN (artifacts: $ART)"
kc get pods >/dev/null || { echo "cluster not reachable; run ./local-up.sh"; exit 1; }

for t in "${TESTS[@]}"; do
    log "--- running test $t ---"
    "test_$t"
done

echo
echo "================= battletest results (run $RUN) ================="
printf '%-28s %-6s %s\n' TEST RESULT DETAIL
fails=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r n s d <<< "$r"
    printf '%-28s %-6s %s\n' "$n" "$s" "$d"
    [ "$s" = PASS ] || fails=$((fails+1))
done
echo "=================================================================="
[ "$fails" = 0 ] && echo "ALL PASS" || echo "$fails FAILURE(S)"
exit "$fails"
