#!/bin/bash
# CAF work-stealing spin-profile sweep, 2c wait=1, 5 reps -- quiet host required.
# No arguments keeps the historical paths, relative to the caller's working directory.
# Optional: caf-sweep.sh <binary> <out-dir>. Output must be absent or empty; tee to a neighbour.
set -u
if [ "$#" -ne 0 ] && [ "$#" -ne 2 ]; then
  printf '%s\n' 'usage: caf-sweep.sh [binary out-dir]' >&2
  exit 2
fi
BIN=${1:-build/final/bin/qvo-caf-savina-ping-pong.exe}
OUT=${2:-build/caf-sweep}
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/../../../tools/spin-sweep.py" caf "$BIN" "$OUT"
