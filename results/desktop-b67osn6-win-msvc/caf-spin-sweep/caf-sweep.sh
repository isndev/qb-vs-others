#!/bin/bash
# CAF work-stealing spin-profile sweep, 2c wait=1, 5 reps -- quiet host required.
BIN=build/final/bin/qvo-caf-savina-ping-pong.exe; OUT=build/caf-sweep; mkdir -p $OUT
run() { # label poll steal
  env QVO_CAF_AGGRESSIVE_POLL=$2 QVO_CAF_STEAL_INTERVAL=$3 "$BIN" --repetitions 5 --warmup 2 --cpus 0,2 --param messages=1000000 --param cores=2 --param wait=1 --out "$OUT/$1.json" >/dev/null 2>"$OUT/$1.err"
  python3 - "$OUT/$1.json" "$1" <<'PY'
import json,sys,statistics
d=json.load(open(sys.argv[1])); w=sorted(d['work_ns']); m=int(d['params']['messages'])
print("%-22s verified=%s p50=%7.1f  min=%7.1f  max=%7.1f" % (sys.argv[2], d['verified'], statistics.median(w)/m, w[0]/m, w[-1]/m))
PY
}
run poll100-steal10   100 10
run poll1e3-steal10   1000 10
run poll1e4-steal10   10000 10
run poll1e5-steal10   100000 10
run poll1e6-steal10   1000000 10
run poll1e4-steal1    10000 1
run poll1e4-steal100  10000 100
run poll1e4-steal1e3  10000 1000
run poll1e6-steal1e6  1000000 1000000
run poll100-steal10-b 100 10
