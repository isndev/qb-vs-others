# QB-196 — the park's wait on WSL2 Debian 13 / g++ 14.2: libev's millisecond, and `epoll_pwait2`

Same host and build flags as the published directories beside this one (i9-12900K under WSL2,
Linux 6.6.87.2-microsoft-standard-WSL2, g++ 14.2.0, Release, `-DQVO_NATIVE_ARCH=OFF`, CAF /
SObjectizer / baseline / controls OFF). Both runs on 2026-09-09, each in its own quiet session —
no Windows bench, no build during a run, 60 s after the last one — with the core pinned to
vCPU 0 by the probe itself. `docs/TUNING.md` §19 is the reading guide.

| file | what |
|---|---|
| `parked-timer-wake.develop.txt` | **the QB-196 instrument on qb `develop` `67f4efb6`** (`tools/probes/parked-timer-wake.cpp`, `qvoprobe-parked-timer-wake`): the same grid as the Windows directory — `latency` ∈ {0, 100, 1000, 10000} µs × `delay` ∈ {100, 1000, 5000} µs, 2000 rounds after 50 warm-ups (400 at 5 ms), then the `idle_spin=0` control — followed by three repetitions of four §10 `parked-io-wake` cells against that section's recorded axis-N figures (level: 37–42 µs where §10 recorded 43–51). The finding: a 100 µs timer on a parked core fires **1010 µs** late at every latency, a 1 ms one 110 µs, a 5 ms one reached through 1 ms parks 357 µs, against 0.1 µs on a spinning core — `epoll_wait`'s whole milliseconds, rounded UP by libev (`EV_TS_TO_MSEC`, `backend_mintime = 1e-3`), plus the thread's 50 µs timer slack. The same ceiling Windows carries under its coalescing. |
| `parked-timer-wake-ab.txt` | **the `epoll_pwait2` A/B**: control qb `develop` `67f4efb6`, candidate qb `perf/epoll-pwait2` `9c401070` (qev `65920ab` mirrored: the epoll backend's blocking wait through `epoll_pwait2`, nanoseconds, probed once per loop), five interleaved repetitions per cell: `parked-timer-wake` at (`latency`, `delay`) = (100, 100), (1000, 100), (1000, 1000), (1000, 5000), (10000, 100), (1000, 100) with `idle_spin=0`, and (0, 100) as the spinning control; `parked-io-wake` at `1000/2000`, `1000/200`, `100/2000`; `io-pass timer` and `io-pass pass` (the NOWAIT pass, §17's floor). Medians of five, p50 in µs: every parked timer cell **1010 / 960 / 110 / 357 → 59–60** (the slack plus a pass, whatever `latency` and `delay`), the spinning control 0.1 → 0.1, the socket wakes 37.8 → 38.0 / 30.8 → 30.2 / 37.7 → 38.0, the NOWAIT passes 25.62 → 25.56 ns and 27.62 → 27.70 ns. |
| `parked-cadence.txt` | **the idle cadence, QB-48's fourth fact** (`tools/probes/parked-cadence.cpp`, `qvoprobe-parked-cadence`, qb `develop` `70354b35`, the `epoll_pwait2` tree): the same grid as the Windows directory. Mean sleep: **163 / 162 µs** at `latency` 100 µs (cv / loop), **1.07 / 1.07 ms** at 1 ms, **10.05 / 10.05 ms** at 10 ms — the value honoured to the 50 µs timer slack plus a pass on either park. |

Not merged into the published tables: both probes are qb-only, and the A/B compares two qb
trees, not frameworks.
