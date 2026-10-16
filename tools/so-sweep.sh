#!/bin/bash
# SObjectizer combined-lock spin-budget sweep (Huly QB-47), the symmetric of the CAF sweep in
# results/<host>/caf-spin-sweep/: savina/ping-pong and savina/counting, cores=2 wait=1, pinned to CPUs 0 and 2,
# 7 repetitions + 2 warmup, 1 000 000 work units, one launch per budget -- QVO_SO_SPIN_WAIT_US in microseconds.
# The profile is measured first and last as the drift control. Quiet host required.
#
#   tools/so-sweep.sh <bin-dir> <out-dir>
# out-dir must be absent or empty. Keep tee's log beside it, not inside it before admission.
set -u
BIN=${1:?bin dir}; OUT=${2:?out dir}
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/spin-sweep.py" so "$BIN" "$OUT"
