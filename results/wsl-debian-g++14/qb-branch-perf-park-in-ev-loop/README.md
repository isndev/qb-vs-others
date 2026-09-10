# qb branch `perf/park-in-ev-loop` (axis N) — WSL2 Debian 13 / g++ 14.2

Same host, CPUs and build flags as the published directories beside this one. **Control** is qb
`develop` at `d1897d3c` (every axis through M and QB-43 merged, the tree axis N branches from),
exported with `git archive` and built clean under `~/qb-f0da` / `~/qvo/ctl-f0da`; **candidate**
is the working tree of `perf/park-in-ev-loop` built under `~/qvo/linux`. Both measured
2026-09-06 in one quiet session per file, the Windows side idle throughout, under `taskset -c 0,2`
— and, for the probe grid that counts, with the core pinned to vCPU 0 and the client thread to
vCPU 2 **by the probe itself** (03:09–03:22 UTC; the A/B and the first, unpinned grid at
02:19–02:47 UTC). `docs/TUNING.md` §10 is the reading guide.

| file | what |
|---|---|
| `parked-io-wake.log` | **the axis-N instrument**, `tools/probes/parked-io-wake.cpp` (`qvoprobe-parked-io-wake`): one echo actor with a TCP listener on a core whose `setLatency` is the cell's `latency`, one raw blocking client that sends a byte, waits for the echo, sleeps `gap`, repeats. 12 cells (`latency` ∈ {0, 100, 1000, 10000} µs × `gap` ∈ {10, 200, 2000} µs), 2000 rounds after 50 warm-ups, **two interleaved passes** (control, candidate, control, candidate per cell). One line per launch, all seven statistics. The finding is the `gap=200us` row at latency 1 ms / 10 ms: control **923 / 9961 µs** p50 (pass 2: 911 / 9954) — the parked core slept its whole `latency` on the condition variable with a socket readable under it — candidate **30.7 / 31.0 µs** (31.2 / 33.7). At `gap=2000us` the control is **100 / 170 / 8154 µs** for latency 100 µs / 1 ms / 10 ms and the candidate **46 / 47 / 51 µs** — the futex wake of a thread that really slept, the §8.2 floor, and the only cost left. The `latency=0` and `gap=10us` cells are the busy-poll floor and move nowhere (p50 17–19 µs, both sides; 25–27 µs at `gap=2000us`, the client's own sleep). |
| `parked-io-wake.unpinned.log` | the FIRST grid, same cells and protocol, measured before the probe pinned its two threads (only `taskset -c 0,2`, 02:19 UTC). Its p50s agree with the pinned grid everywhere (914 / 9966 → 30.5 / 31.8 µs on the headline row), which is why the finding was never in doubt; kept because its `latency=0` tails are NOT the hypervisor, as first read, but the pinning trap Windows exposed outright (`results/desktop-b67osn6-win-msvc/qb-branch-perf-park-in-ev-loop/`): with the client free to land on the core's vCPU the busy-poll cells carried a p99 of 1.2–3.1 ms on both sides, and the pinned grid's p99 on the same cells is 42–150 µs. Linux's load balancer hid the trap from the medians and left it in the tails. |
| `ab/{ctl,cand}__<cell>-r{1,2,3}.json` | **the regression gate**: `qvo-qb-savina-ping-pong`, 7 repetitions + 2 warmup, 1 000 000 round trips, three reps per cell interleaved control/candidate. Cells: `1c-w1` (one core, spin), `2c-w0` (two cores, park at the default idle-spin floor), `2c-w1` (two cores, spin), `2c-w0-QVO_QB_IDLE_SPIN_US=0` (two cores, the core blocks on its first idle pass — §8.2's experiment). Per-rep p50 in ns per round trip, control → candidate: **1c-w1** 66.6 / 67.2 / 67.1 → 67.3 / 67.9 / 67.5; **2c-w0** 230.0 / 228.9 / 216.1 → 236.8 / 219.3 / 217.9; **2c-w1** 238.0 / 223.2 / 211.1 → 219.4 / 238.8 / 221.8; **2c-w0 idle-spin 0** 27 332 / 27 624 / 26 910 → 27 357 / 27 350 / 27 331 (the WSL2 futex-wake floor, both sides, as §8.2 records). No cell moved outside its own rep-to-rep spread; the one-core cell's +0.4 ns (0.6 %) is below the 1 ns this benchmark resolves and is the candidate's `has_work()` gate being evaluated once per idle pass on a core that never parks. |

Ping-pong carries no io watcher, so it cannot exercise the new park path at all: on this benchmark
the candidate's parked core takes exactly the control's condition-variable branch, and the A/B
is the proof that the tri-state `_parked` and the acquire re-read in `notify()` cost the hot
path nothing. What axis N changes is measured only by the probe.

These are NOT merged into the published tables — the probe is qb-only, and the ping-pong cells
are the control's and the candidate's, not a framework comparison.
