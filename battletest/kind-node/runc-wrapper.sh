#!/bin/bash
# runc wrapper for sandboxed hosts that deny lowering /proc/self/oom_score_adj
# (e.g. nested/gVisor-ish container sandboxes). kubelet asks for negative
# oomScoreAdj on every pod, which makes runc's nsexec die with
# "can't get final child's PID from pipe: EOF". Strip negative oomScoreAdj
# from the bundle config before delegating to the real runc.
bundle=""
prev=""
for a in "$@"; do
    if [ "$prev" = "--bundle" ] || [ "$prev" = "-b" ]; then bundle="$a"; fi
    prev="$a"
done
[ -z "$bundle" ] && bundle="$PWD"
if [ -f "$bundle/config.json" ]; then
    python3 - "$bundle/config.json" <<'PYEOF' 2>/dev/null || true
import json, sys
p = sys.argv[1]
s = json.load(open(p))
if s.get("process", {}).get("oomScoreAdj", 0) < 0:
    del s["process"]["oomScoreAdj"]
    json.dump(s, open(p, "w"))
PYEOF
fi
exec /usr/local/sbin/runc.real "$@"
