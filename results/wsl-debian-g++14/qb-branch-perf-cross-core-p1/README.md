# qb-branch-perf-cross-core-p1 — WSL2 g++-14 (2026-10-11)

The receipts of Huly QB-979 (the flush's stall rule) and QB-1010 (cigsmok's two-core placement), measured in
one reserved session (`idle-record.txt`: the host's load and process list before the timing) on the i9-12900K
guest, CPUs 0 and 2, g++ 14.2.0 `-O3 -DNDEBUG`, the qb-vs-others tree at `622ad51f`. Read with
`docs/TUNING.md` §21.7 and §21.8.

Control: qb `9aebf2fc` (develop). The rule: qb branch `perf/cross-core-p1`, three forms measured against the
same control copies — form 1 bailed on inbound work alone (`cells-form1/`), form 2 called a stall on the first
unmoved refusal (`cells-form2/`), the rule calls it on the second (`cells/`).

| directory | what | protocol |
|---|---|---|
| `cells*/` | `summary.txt` (per cell: each copy's median over rounds, the group's median of copy medians, the delta, the control's copy spread), `orders.txt` (the seeded order of every round), `meta.txt`; `cells/nqueens-modes.txt` the slow-mode share per repetition of the three forms | `ab-qvo-cells-wsl.sh`: 3 physical copies a side, seeded Fisher–Yates order per round over all 6, 8 rounds of 3 + 1, 60 s of quiet first |
| `bench-plain/`, `bench-align/` | `dev/bench/ab.py` over the four event-path binaries, the production flags and the `-falign-functions/-loops/-jumps=64` pair (the layout control); `summary.txt` the tool's table, `ab-meta.json` its seed, rounds, copies | 3 copies, 8 rounds, seeded, `--reps 3`, 60 s quiet |
| `probe/` | the cigsmok arbiter probe (QB-1010, §21.7 instrument 4): `cigsmok-probe.stderr` the wait per round by `[previous smoker's core][this smoker's core]`, four repetitions; its result document | one launch of 3 + 1, CPUs 0,2; a probe build, never a cell |
| `census/` | `sends_blocked` / `loop_passes` / events per core on every qb Savina shape at 1c and 2c, one launch each, the rule's build with a stderr print | counters, not time |
| `callgrind/` | the inclusive Ir tables at one core with `smoke=1` (§21.7 instrument 2): qb `73018675`, qb 3.2.1, SObjectizer 5.8.5.1, the floor | callgrind 3.24, `--cpus 0`, one repetition |

The per-run result documents (672 per side per cells run, 24 per side per bench run) stayed in the session
directory (`~/p1` on the guest); the summaries above are computed from them by
`results/wsl-debian-g++14/qb-branch-qb65-signal-generations/scripts/ab-summarize-groups.py`.
