#!/bin/bash
# QB-1171: build qvoprobe-ask-any-scatter against ONE qb tree, its own qb libraries (add_subdirectory of that tree),
# Release, Apple clang, the flags of one alignment family. Usage: build-probe.sh <name> <qb-tree> [extra CXX flags]
set -euo pipefail
S=/private/tmp/claude-501/-Users-mbelhadi-repo-qb-dev--claude-worktrees-qb-1171-ask-trim-963d9d/cab4149a-7b98-4639-a892-dcfcdcc8aff0/scratchpad
name=$1; tree=$2; flags=${3:-}
B=$S/ab/b-$name
M=/private/tmp/claude-501/-Users-mbelhadi-repo-qb-dev/BUSY-bench
[ -e "$M" ] && { echo "marker present: refusing to build"; exit 2; }
rm -rf "$B"
cmake -S $S/qbvo -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DQVO_QB_DIR="$tree" \
  -DQVO_WITH_CAF=OFF -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF -DQVO_BUILD_CONTROLS=OFF -DQVO_BUILD_PROBES=ON \
  -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_C_FLAGS="$flags" > "$S/ab/cfg-$name.log" 2>&1
cmake --build "$B" -j 6 --target qvoprobe-ask-any-scatter > "$S/ab/build-$name.log" 2>&1
echo "$name warnings: $(grep -ci 'warning:' "$S/ab/build-$name.log" || true)"
ls -l "$B/bin/qvoprobe-ask-any-scatter"
"$B/bin/qvoprobe-ask-any-scatter" same 1 20000
