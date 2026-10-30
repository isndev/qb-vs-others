#!/bin/bash
# QB-1171 on the M4: ONE placement family, control (develop ea0760f6) against trim 2, the pilot's cells:
# same N = 1, 2, 4, 8 and cross N = 1, 2, 4, 3 physical copies a side, 8 seeded rounds, 200 000 asks, --no-pin.
# Usage: campaign.sh <family> [seed]   (run under quiet-run.sh: it holds the shared marker)
set -u
S=/private/tmp/claude-501/-Users-mbelhadi-repo-qb-dev--claude-worktrees-qb-1171-ask-trim-963d9d/cab4149a-7b98-4639-a892-dcfcdcc8aff0/scratchpad
fam=$1; seed=${2:-7101}
R=$S/qbvo/results/macbook-m4pro-macos-clang21/qb-branch-develop/ask-registry-trims
cd $S/qbvo
common=(--side develop=$S/ab/b-base-$fam/bin/qvoprobe-ask-any-scatter --side trim2=$S/ab/b-trim2-$fam/bin/qvoprobe-ask-any-scatter
        --no-pin --asks 200000 --copies 3 --rounds 8 --seed $seed --quiet 20)
rm -rf "$R/$fam-same" "$R/$fam-cross"
python3 tools/scatter-ab.py --out "$R/$fam-same" "${common[@]}" --responders 1,2,4,8 --mode same --tag "M4 Pro QB-1171 trim 2, family $fam, same"
echo "$fam same rc=$?"
python3 tools/scatter-ab.py --out "$R/$fam-cross" "${common[@]}" --responders 1,2,4 --mode cross --tag "M4 Pro QB-1171 trim 2, family $fam, cross"
echo "$fam cross rc=$?"
