#!/bin/bash
# QB-1171 point 4 on the M4: shipped 3.3.0 -> c16f1fa9 (QB-1114) -> b67ac7b8 (QB-1154) -> develop ea0760f6, N = 1 alone,
# cross and same, 3 physical copies a side, 8 seeded rounds, 200 000 asks, --no-pin, default flags.
# Usage: bisect.sh <pass> [seed]   (run under quiet-run.sh)
set -u
S=/private/tmp/claude-501/-Users-mbelhadi-repo-qb-dev--claude-worktrees-qb-1171-ask-trim-963d9d/cab4149a-7b98-4639-a892-dcfcdcc8aff0/scratchpad
pass=$1; seed=${2:-7101}
R=$S/qbvo/results/macbook-m4pro-macos-clang21/qb-branch-develop/ask-registry-trims
cd $S/qbvo
common=(--side v330=$S/ab/b-bis-v3.3.0/bin/qvoprobe-ask-any-scatter --side c16f=$S/ab/b-bis-c16f1fa9/bin/qvoprobe-ask-any-scatter
        --side b67a=$S/ab/b-bis-b67ac7b8/bin/qvoprobe-ask-any-scatter --side tip=$S/ab/b-base-f0/bin/qvoprobe-ask-any-scatter
        --no-pin --asks 200000 --copies 3 --rounds 8 --seed $seed --quiet 20)
for mode in cross same; do
  rm -rf "$R/bisect-n1-$pass-$mode"
  python3 tools/scatter-ab.py --out "$R/bisect-n1-$pass-$mode" "${common[@]}" --responders 1 --mode $mode --tag "M4 Pro QB-1171 point 4, N = 1 bisection, $mode, pass $pass"
  echo "bisect $pass $mode rc=$?"
done
