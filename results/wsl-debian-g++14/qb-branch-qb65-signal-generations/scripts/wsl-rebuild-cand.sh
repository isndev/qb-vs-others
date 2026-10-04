#!/bin/bash
# Rebuild the WSL QB-65 candidate from the COMMIT (72f1fb5f) -- the tree measured before was an earlier cut -- and
# assert the four signal files are byte-identical to the commit before anything is measured on it.
set -u
QB=/mnt/d/repo/qb-dev/qb; QVO=/mnt/d/repo/qb-dev/qb-vs-others; CAND=72f1fb5f
ts() { date -u +%H:%M:%S; }
rm -rf ~/qb-65-cand ~/qvo-65-cand; mkdir -p ~/qb-65-cand/src ~/qvo-65-cand/src
git -C $QB archive $CAND | tar -x -C ~/qb-65-cand/src
git -C $QVO archive HEAD | tar -x -C ~/qvo-65-cand/src
for f in src/qb/core/VirtualCore.cpp src/qb/core/VirtualCore.h src/qb/core/Main.cpp src/qb/core/Main.h; do
  git -C $QB show $CAND:$f | cmp -s - ~/qb-65-cand/src/$f && echo "cand same $f" || { echo "cand DIFF $f"; exit 1; }
done
cmake -S ~/qvo-65-cand/src -B ~/qvo-65-cand/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQVO_QB_DIR=$HOME/qb-65-cand/src \
      -DQVO_WITH_CAF=OFF -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF > /tmp/cfg-cand.log 2>&1 || { tail -5 /tmp/cfg-cand.log; exit 1; }
cmake --build ~/qvo-65-cand/build -j 16 --target qvo-qb-savina-ping-pong qvo-qb-savina-thread-ring qvo-qb-savina-fib \
      qvo-qb-savina-big qvoprobe-pass-cost qvoprobe-ask-cost > /tmp/bld-cand.log 2>&1 || { grep -m5 -A5 error /tmp/bld-cand.log; exit 1; }
echo "=== cand rebuilt from $CAND $(ts) warnings=$(grep -c 'warning:' /tmp/bld-cand.log)"
