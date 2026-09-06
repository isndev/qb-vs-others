# qb branch `perf/park-in-ev-loop` (axis N) — Windows 11 / MSVC 19.51

Same host and build flags as the published directories beside this one. **Control** is qb
`develop` at `f0da4e32` (every axis through M and QB-43 merged, the tree axis N branches from),
exported with `git archive` and built clean under `build/ctl-f0da-win/`; **candidate** is the
working tree of `perf/park-in-ev-loop` built under `build/final/`. Both measured 2026-09-06
03:02–03:27 UTC in one quiet session — no build, no test suite, the WSL2 side idle throughout —
with the process mask set to CPUs 0 and 2 and, for the probe, the core pinned to CPU 0 and the
client thread to CPU 2 **by the probe itself**. `docs/TUNING.md` §10 is the reading guide.

| file | what |
|---|---|
| `parked-io-wake.log` | **the axis-N instrument**, `tools/probes/parked-io-wake.cpp` (`qvoprobe-parked-io-wake`): one echo actor with a TCP listener on a core whose `setLatency` is the cell's `latency`, one raw blocking client that sends a line, waits for the echo, sleeps `gap`, repeats. 12 cells (`latency` ∈ {0, 100, 1000, 10000} µs × `gap` ∈ {10, 200, 2000} µs), 2000 rounds after 50 warm-ups, **two interleaved passes** (control, candidate, control, candidate per cell). One line per launch, all seven statistics. The finding is every cell whose `gap` exceeds the idle-spin floor on a core that can park: control p50 **1221 / 898 µs** at latency 100 µs, **2215 / 2788 µs** at 1 ms, **15 550 / 15 567 µs** at 10 ms (`gap=200us`), and **2112 / 2050, 11 621 / 10 545, 13 774 / 13 780 µs** at `gap=2000us` — the parked core slept on its condition variable with a socket readable under it, and on Windows MSVC's `wait_for` lands on the 15.6 ms scheduler tick (§5), so even `latency=100us` cost milliseconds. Candidate: **24–27 µs** at `gap=200us` on all three latencies, **62 / 62, 62 / 69, 135 / 134 µs** at `gap=2000us` — the OS wake of a thread that really slept 2 ms, deeper the longer the cap let it sleep. The `latency=0` and `gap=10us` cells are the busy-poll floor and move nowhere (p50 18–20 µs, both sides; 28–29 µs at `gap=2000us` because the client's own 2 ms sleep costs it a wake too). |
| `ab/{ctl,cand}__<cell>-r{1,2,3}.json` | **the regression gate**: `qvo-qb-savina-ping-pong`, 7 repetitions + 2 warmup, 1 000 000 round trips, three reps per cell interleaved control/candidate. Cells: `1c-w1` (one core, spin), `2c-w0` (two cores, park at the default idle-spin floor), `2c-w1` (two cores, spin), `2c-w0-QVO_QB_IDLE_SPIN_US=0` (two cores, the core blocks on its first idle pass — §8.2's experiment). Per-rep p50 in ns per round trip, control → candidate: **1c-w1** 84.2 / 79.7 / 81.2 → 80.8 / 80.3 / 80.5; **2c-w0** 292.0 / 286.2 / 304.8 → 274.2 / 294.7 / 270.0; **2c-w1** 351.1 / 309.8 / 275.4 → 270.3 / 330.4 / 249.3; **2c-w0 idle-spin 0** 390.3 / 463.8 / 355.8 → 393.1 / 421.1 / 422.0 (the Windows handshake-absorbed park of §8.2, both sides). No cell moved outside its own rep-to-rep spread — the two-core cells are bimodal per launch on this host, as every Windows two-core document beside this one records, and the candidate's three reps sit inside the control's range in all three. |

**The pinning is the instrument's, not the launcher's, and the difference was measured.** The
first Windows run set only the process-wide affinity mask ({0, 2}); the scheduler then placed the
client thread on the core's own CPU for a per-launch random fraction of the rounds, and a client
spinning its gap on the core's CPU keeps the core from ever seeing its idle-spin floor elapse — so
it never parked and the control's `latency=1000 gap=200` cell read p50 19–20 µs with a p90
anywhere between 23 µs and 1.8 ms from one launch to the next. Every figure above is from the
probe as committed, which pins the core to CPU 0 and the client to CPU 2 itself. The WSL2
directory's first grid predates that edit; Linux's load balancer spread the two busy threads and
hid the trap from every p50 but left it in the `latency=0` tails, which is why that directory
keeps the unpinned log beside the pinned re-measurement (`parked-io-wake.unpinned.log`).

Ping-pong carries no io watcher, so it cannot exercise the new park path at all: on this benchmark
the candidate's parked core takes exactly the control's condition-variable branch, and the A/B
is the proof that the tri-state `_parked` and the acquire re-read in `notify()` cost the hot
path nothing. What axis N changes is measured only by the probe.

These are NOT merged into the published tables — the probe is qb-only, and the ping-pong cells
are the control's and the candidate's, not a framework comparison.
