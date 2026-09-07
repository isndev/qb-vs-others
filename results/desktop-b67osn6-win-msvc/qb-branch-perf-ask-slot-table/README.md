# qb branch `perf/ask-slot-table` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-178** (the WSL2 half, with the profile that drove the
branch, is `../../wsl-debian-g++14/qb-branch-perf-ask-slot-table/`): the qb branch that turns the
pending-ask registry into a slot table the correlation id indexes, measured against the `develop`
it forks from on the one Savina shape that sits on the request path — `savina/bank-transaction`
(one teller, 1 000 accounts, 50 000 transfers, each a `qb::ask<Deposit>` nested in a request;
`benchmarks/savina/bank-transaction.md`) — plus the two `dev/bench` ask cells that gate it
(`qb-core-bench-ask-roundtrip`, `qb-core-bench-ask-all-fanout`). Same host, CPUs and build flags
as the published directories beside this one: `/O2 /DNDEBUG`, CPUs 0 and 2 (two P-cores),
**9 repetitions + 2 warmup**, qb-only builds (`build/ab178-cand`, `build/ab178-ctl`:
`-DQVO_WITH_CAF=OFF -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF`; the field is in
`../savina-bank-transaction/`), candidate / control / candidate in ONE quiet session on
2026-09-07, **06:28:27–06:28:29 UTC**, no build, no test suite and no WSL measurement running
anywhere on the host (the WSL2 session B had ended at 06:19:50 UTC). The candidate was built
against a git worktree of `qb/` at the branch head (`D:\repo\qb-178`, 0 dirty); the control
against `qb/` itself at `ba9b1a81` (0 dirty). The branch is measured at its head only here — the
intermediate `dfa303ec` was A/B'd on WSL2, where the profile lives.

| directory | qb at | what |
|---|---|---|
| `grid-82ea03a8/`, `grid-82ea03a8-pass2/` | `develop` `ba9b1a81` + `dfa303ec` (the slot table: `[core:16][generation:26][slot:22]`, FIFO free list, no hashing) + **`82ea03a8`** (take binds the slot in the same call, `finish()` runs once, the owner is built before the send) — **the candidate**, measured first and third | **4 cells**, qb only, all verified: bank-transaction × {1c-spin, 1c-park, 2c-spin, 2c-park}. |
| `grid-ba9b1a81/` | `develop` `ba9b1a81` — **the control**, measured second | same 4 cells, same session. |
| `bench/` | the two `dev/bench` ask binaries (standalone `cmake -S <qb>` Release, `-DQB_ENABLE_NATIVE_ARCH=ON`, `vcpkg-optional.cmake` toolchain, `-DQB_INSTALL=OFF` because this host has no system zlib and a source-built one cannot be exported), candidate and control alternated three times (`cand-N-*` / `ctl-N-*`), 5 repetitions each, aggregates only | 06:32:59–06:34:40 UTC, right after the grids; medians of the three medians below. |

None of the grids is merged into the published tables (`../savina-bank-transaction/` renders
shipped 3.1.0 and the two `develop` builds of §12); the branch joins them when the 3.2.0 grid is
measured for all seven shapes in one session.

## Control against the candidate, same session (p50 ms)

| config | `ba9b1a81` | **`82ea03a8`** | pass 2 | Δ | min: ctl / cand / pass 2 |
|---|---|---|---|---|---|
| 1c-spin | 12.222 | **11.988** | 12.070 | -1.9 % / -1.2 % | 11.606 / 11.752 / 11.892 |
| 1c-park | 12.107 | **12.119** | 12.248 | +0.1 % / +1.2 % | 11.777 / 11.778 / 11.823 |
| 2c-spin | 7.424 | **7.106** | 7.533 | -4.3 % / +1.5 % | 6.993 / 6.911 / 7.157 |
| 2c-park | 7.791 | **7.181** | 7.090 | -7.8 % / -9.0 % | 6.998 / 6.928 / 6.918 |

**On MSVC the branch is inside the spread on every cell.** The 1c pair moves −1.9 % / −1.2 %
on spin and +0.1 % / +1.2 % on park while the control's minimum is the lowest of the three on
both; the 2c-park p50 moves −8 / −9 % but its three minima sit within 1.2 % of each other
(6.998 / 6.928 / 6.918), which is a wide control median rather than a faster candidate — the
same Windows 2c behaviour `docs/TUNING.md` §12.4 already records. Per transfer on one core,
spin, MSVC is at ≈ 240 ns against g++'s 144, and the ≈ 9–10 ns per ask the WSL2 pair measured
would be 4 % here if MSVC paid the same thing; it does not show. The reading the WSL2 profile
supports, recorded and not profiled on this host: the cost `82ea03a8` removes on g++ is the
CALL — a `thread_local` with a destructor is a `__tls_init` guard check per access there, and
the path made five out-of-line trips per ask — while MSVC runs dynamic TLS initialisation once
at thread start through its TLS callback and has no per-access guard to skip, so the same three
calls fewer buy correspondingly less. No Windows profile was taken; a `perf`-grade sample on
MSVC would be `xperf`/`vtune`, and none is in this repository's protocol.

| `dev/bench` cell (median of 3 medians) | ctl `ba9b1a81` → `82ea03a8` |
|---|---|
| `Ask_RoundTrip_SameCore` real_time | 36.8 → 37.3 ms (+1.5 %) |
| `Ask_RoundTrip_CrossCore` real_time | 45.7 → 46.4 ms (+1.5 %) |
| `Ask_Scatter<All>/responders:8` | 79.5 → 79.2 ms (-0.5 %) |
| `Ask_Scatter<Any>/responders:8` | 75.0 → 74.7 ms (-0.5 %) |

Flat, as on WSL2 for the two round-trip cells (a round trip there is ≈ 740 ns of loop pass,
timer and latency sampling, so a few ns per ask is inside the cell's spread). One outlier is on
disk and excluded by the median of medians, not by hand: `cand-1-ask-roundtrip.json`'s
`SameCore` median is 56.4 ms against 36.1 / 37.3 on the candidate's other two passes and
36.4–37.2 on the control's three — the first launch of the session, the shape a P-core clock
ramp leaves. The WSL2 scatter-`All` −9 / −12 % does not reproduce here (−0.5 %), which is what a
recorded-only cell with a 16–20 % spread is expected to do.

Suites at the head, standalone `cmake -S qb`, 0 warnings: Windows/MSVC 19.51 Release
188/188/0 (`D:\repo\qb-178-win\release.*.log`); WSL2 g++-14 Release / ASan+UBSan / TSan
192/192/0 each.
