#!/bin/bash
# QB-1171: one SHORT measurement job (<= 5 min) under the shared marker, marker taken FIRST so that no new build or
# suite starts, then up to 10 min for the running ones to drain (executable names, never command lines); the
# marker is held 15 min at most and released in a trap. If the host does not drain, nothing is measured (exit 4).
# Usage: marker-first.sh <log> <command> [args...]
set -u
M=/private/tmp/claude-501/-Users-mbelhadi-repo-qb-dev/BUSY-bench
log=$1; shift
busy() { ps -Ao comm= | grep -qE '(^|/)(clang|clang\+\+|c\+\+|cc|ld|ninja|ctest|make)$|-test-'; }
while [ -e "$M" ]; do sleep 2; done
if ! ( set -o noclobber; echo "QB-1171 $$ $(date +%T) (marker-first: draining, then <= 5 min of probe)" > "$M" ) 2>/dev/null; then
  echo "[$(date +%T)] marker race lost: nothing measured" | tee -a "$log"; exit 5
fi
trap 'rm -f "$M"; echo "[$(date +%T)] marker released" >> "$log"' EXIT
t0=$(date +%s); quiet=0
while :; do
  if ! busy; then quiet=$((quiet + 1)); [ $quiet -ge 3 ] && break; else quiet=0; fi
  [ $(( $(date +%s) - t0 )) -gt 600 ] && { echo "[$(date +%T)] marker held, host did not drain in 10 min: nothing measured" | tee -a "$log"; exit 4; }
  sleep 5
done
{
  echo "[$(date +%T)] drained after $(( $(date +%s) - t0 )) s (quiet=$quiet) under the marker"
  echo "  host: $(uptime | sed 's/.*load/load/'); top 4: $(ps -Ao pcpu,comm -r | sed -n 2,5p | awk '{printf "%s%% %s; ", $1, $NF}')"
} | tee -a "$log"
"$@" >> "$log" 2>&1
rc=$?
echo "[$(date +%T)] command rc=$rc; host: $(uptime | sed 's/.*load/load/'); top 4: $(ps -Ao pcpu,comm -r | sed -n 2,5p | awk '{printf "%s%% %s; ", $1, $NF}')" | tee -a "$log"
exit $rc
