#!/bin/bash
# QB-1171: instructions retired per ask_any on the M4 (the arm64 stand-in for callgrind's Ir): /usr/bin/time -l over a
# whole probe process at 50 000 and 200 000 asks, the difference divided by 150 000 (start, stop and idle cancel),
# `same` placement only (one core: no spinning peer adds instructions), 5 launches per point, the median.
# Usage: instr.sh <side=probe> [<side=probe> ...]
set -u
for sp in "$@"; do
  side=${sp%%=*}; exe=${sp#*=}
  for n in 1 4; do
    for asks in 50000 200000; do
      vals=()
      for k in 1 2 3 4 5; do
        v=$( { /usr/bin/time -l "$exe" same $n $asks >/dev/null; } 2>&1 | awk '/instructions retired/ {print $1}')
        vals+=("$v")
      done
      med=$(printf '%s\n' "${vals[@]}" | sort -n | sed -n 3p)
      echo "$side N=$n asks=$asks instructions_median=$med all=${vals[*]}"
      eval "I_${n}_${asks}=$med"
    done
    eval "echo \"$side N=$n per_ask_any=\$(( (I_${n}_200000 - I_${n}_50000) / 150000 )) instructions\""
  done
done
