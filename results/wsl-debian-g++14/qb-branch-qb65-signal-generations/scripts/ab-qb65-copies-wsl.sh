#!/bin/bash
# QB-65 on WSL2 g++-14, the multi-copy census: 4 byte-identical copies of the parent's binaries and 4 of the
# candidate's, each its own file (its own physical pages: a single copy's A/A sat -3.4 % off on ping-pong 2c), random
# order per round over all 8 (seeded), 40 rounds. Group = median of the copies. Reuses the A/B builds ~/qvo-65-*.
set -u
ROUNDS=${1:-40}; SEED=${2:-6570}
S=<scratch>
QVO=/mnt/d/repo/qb-dev/qb-vs-others
OUT=$QVO/results/wsl-debian-g++14/qb-branch-qb65-signal-generations; R=$OUT/copies
rm -rf "$R"; mkdir -p "$R"; cd /tmp
ts() { date -u +%H:%M:%S; }
TAGS=""
for g in ctl cand; do for c in 1 2 3 4; do
  t=${g}_$c; TAGS="$TAGS $t"; rm -rf ~/qvo-65-copies/$t; mkdir -p ~/qvo-65-copies/$t
  cp -a ~/qvo-65-$g/build/bin/. ~/qvo-65-copies/$t/
done; done
python3 -c "
import random; r = random.Random($SEED); tags = '$TAGS'.split()
for _ in range($ROUNDS):
    t = list(tags); r.shuffle(t); print(' '.join(t))" > "$R/orders.txt"
echo "=== quiet 60 s $(ts)"; sleep 60
echo "=== copies $(ts) seed=$SEED load=$(cut -d' ' -f1-3 /proc/loadavg)"
round=0
while read -r order; do
  round=$((round + 1))
  for tag in $order; do B=~/qvo-65-copies/$tag
    for cell in "ping-pong cores=1 wait=1" "thread-ring cores=1 wait=1" "ping-pong cores=2 wait=1" "thread-ring cores=2 wait=1"; do set -- $cell
      $B/qvo-qb-savina-$1 --repetitions 3 --warmup 1 --cpus 0,2 --out $R/$1-$2-$3@$tag@$round.json --param $2 --param $3 >/dev/null 2>&1
    done
  done
done < "$R/orders.txt"
python3 "$S/ab-summarize-groups.py" "$R" | tee "$R/summary.txt"
: > "$R/probe.txt"
for pr in 1 2 3 4 5 6; do
  for tag in $(python3 -c "import random; r = random.Random($SEED + $pr); t = '$TAGS'.split(); r.shuffle(t); print(' '.join(t))"); do
    B=~/qvo-65-copies/$tag
    echo "$tag pass-cost k=1 $($B/qvoprobe-pass-cost 1 2 2 2>&1 | tail -1)" >> "$R/probe.txt"
    echo "$tag ask-cost ask $($B/qvoprobe-ask-cost ask 2 2 2>&1 | tail -1)" >> "$R/probe.txt"
  done
done
python3 - "$R/probe.txt" <<'PY' | tee "$R/probe-summary.txt"
import sys, re, statistics, collections
d = collections.defaultdict(list)
for ln in open(sys.argv[1]):
    p = ln.split(); m = re.search(r"ns_per_(?:pass|trip)=([0-9.]+)", ln)
    if len(p) >= 3 and m:
        d[(p[1] + " " + p[2], p[0].split("_")[0])].append(float(m.group(1)))
for probe in sorted({k[0] for k in d}):
    c = sorted(d[(probe, "ctl")]); k = sorted(d[(probe, "cand")]); mc = statistics.median(c); mk = statistics.median(k)
    print("%-16s ctl %7.2f  cand %7.2f  %+5.1f%%   [%.2f..%.2f] / [%.2f..%.2f]" % (probe, mc, mk, (mk - mc) / mc * 100, c[0], c[-1], k[0], k[-1]))
PY
echo "=== AB-QB65-COPIES-WSL-DONE $(ts) load=$(cut -d' ' -f1-3 /proc/loadavg)"
