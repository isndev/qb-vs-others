#!/bin/bash
# SObjectizer combined-lock spin-budget sweep (Huly QB-47), the symmetric of the CAF sweep in
# results/<host>/caf-spin-sweep/: savina/ping-pong and savina/counting, cores=2 wait=1, pinned to CPUs 0 and 2,
# 7 repetitions + 2 warmup, 1 000 000 messages, one launch per budget -- QVO_SO_SPIN_WAIT_US in microseconds
# (0 = simple_lock_factory, the lower bound; unset = the adapter's 10 s profile, the published cell), the
# adapter's profile measured first and again last as the drift control. Quiet host required.
#
#   tools/so-sweep.sh <bin-dir> <out-dir>          (bin-dir holds qvo-sobjectizer-savina-{ping-pong,counting}[.exe])
set -u
BIN=${1:?bin dir}; OUT=${2:?out dir}; mkdir -p "$OUT"
EXT=""; [ -f "$BIN/qvo-sobjectizer-savina-ping-pong.exe" ] && EXT=".exe"
run() { # shape label [budget_us]
  local shape=$1 label=$2 budget=${3:-}
  local exe="$BIN/qvo-sobjectizer-savina-$shape$EXT"
  if [ -n "$budget" ]; then
    env QVO_SO_SPIN_WAIT_US="$budget" "$exe" --repetitions 7 --warmup 2 --cpus 0,2 --param messages=1000000 --param cores=2 --param wait=1 --out "$OUT/$shape-$label.json" >/dev/null 2>"$OUT/$shape-$label.err"
  else
    "$exe" --repetitions 7 --warmup 2 --cpus 0,2 --param messages=1000000 --param cores=2 --param wait=1 --out "$OUT/$shape-$label.json" >/dev/null 2>"$OUT/$shape-$label.err"
  fi
  python3 - "$OUT/$shape-$label.json" "$shape-$label" <<'PY'
import json, sys, statistics
d = json.load(open(sys.argv[1])); w = sorted(d['work_ns']); m = int(d['params']['messages'])
sweep = any(c.startswith("SWEEP DOCUMENT") for c in d.get('caveats', []))
print("%-28s verified=%s sweep-caveat=%s p50=%7.1f  min=%7.1f  max=%7.1f ns/msg" % (sys.argv[2], d['verified'], sweep, statistics.median(w)/m, w[0]/m, w[-1]/m))
PY
}
for shape in ping-pong counting; do
  run $shape profile-10s
  run $shape simple-lock 0
  run $shape wait1us 1
  run $shape wait10us 10
  run $shape wait100us 100
  run $shape wait1ms-default 1000
  run $shape wait10ms 10000
  run $shape wait100ms 100000
  run $shape wait10s-b 10000000
  run $shape profile-10s-b
done
