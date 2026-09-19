# qev branch `perf/nowait-pass-floor` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-188** (the non-blocking pass at its floor) and
**QB-193** (the loop's clocks on Windows: `QueryPerformanceCounter` and
`GetSystemTimePreciseAsFileTime` instead of the system tick), measured on 2026-09-08 in one
quiet session (load 2, no build, the WSL2 side idle) against the batch the branch forks from
(qb `5de4b363` with qev `ea565d1`, i.e. QB-185 + QB-187): the control is
`D:\repo\qb-ctl-5de4b363` (a clean clone, 0 dirty), the candidate the working tree with the
branch's `src/qb/ev/` copy, and a third tree carries **QB-188 alone** (`D:\repo\qb-188only-win`:
the same clone with the branch's `ev.c` / `ev.h` but not its `ev_win32.c`), so the two changes
are told apart. Same flags as the published directories (`/O2 /Ob2 /DNDEBUG`), CPU 0,
candidate and control alternated five times, 1.5 s per point.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-ask-cost` `ask` and one- and 64-chunk `ask_stream` **with a 500 ms timeout**, the untimed `ask`, `push`; `qvoprobe-pass-cost` k = 1 — the full branch (QB-188 + QB-193) vs control × 5. 03:13:47–03:15:18 UTC. |
| `probe-188-only.txt` | the three timed shapes, QB-188 alone vs control × 5. 03:17:59 UTC. |
| `bench-pass.txt` | qev's `bench/bench-pass.c` on MSVC, qev `ea565d1` vs the branch, × 5 (the `fd` shapes need a POSIX pipe and do not run here). |

None of it is merged into the published tables.

## What one non-blocking pass costs on MSVC (ns, medians of five)

| shape | qev `ea565d1` (QB-187) | QB-188 alone | **QB-188 + QB-193** |
|---|---:|---:|---:|
| empty loop | 19.7 | 16.7 | 31.2 |
| one far timer | 19.7 | 15.8 | 31.4 |
| `ev_now_update` | 3.4 | 3.4 | 19.2 |
| `ev_timer_start` + `stop` | 4.0 | — | 4.2 |
| the three with `ev_now_update` | 5.8 | 5.5 | 24.4 |

(The QB-188-alone column is the earlier run of the same binary before the clock change, three
launches.) The MSVC pass was cheap for a bad reason: its clock was `GetSystemTimeAsFileTime`,
a 3 ns memory read of the system tick that steps every 2.2 ms on this host (93 distinct
values in 200 ms; `NtQueryTimerResolution` reports 15.625 ms) and is not monotonic — and
MSVC having no `clock_gettime`, it was the loop's monotonic clock too, so every libev timer
was judged at that granularity. `QueryPerformanceCounter` costs ~16 ns (the TSC read, the
same floor the vDSO has on Linux), and that is the honest price of the timers being right.

## The probes (ns per round trip, one core)

| probe | control (QB-185 + 187) | QB-188 alone | **QB-188 + QB-193** |
|---|---:|---:|---:|
| **ask with a 500 ms timeout** | 123.8 | **114.6** (−7 %) | 166.4 (+34 %) |
| stream, 1 chunk, with a 500 ms timeout | 327.1 | 319.5 (−2 %) | 371.3 (+14 %) |
| stream, 64 chunks, with a 500 ms timeout (per chunk) | 75.8 | 76.0 | 76.9 |
| ask (no timeout) | 71.2 | — | 70.1 |
| push | 34.2 | — | 33.2 |
| `pass-cost` k = 1 | 16.3 | — | 16.4 |

The timed ask pays the precise clock four times — three NOWAIT passes and the
`ev_now_update` before the timer is armed (qb refreshes the loop's clock before every
`ev_timer_start`, as libev's manual asks) — ~13 ns each. Those four reads are exactly what the
next two steps of the qev programme remove: the deadline list (QB-189) takes the libev timer
off the request path, and the embedder's own reading handed to the loop (QB-190) takes the
pass's read off a core that already made one. What Windows gets in exchange today: qev's loop
suite **39 run / 0 failed / 3 skipped** on MSVC where the shipped code measured 27 / 2 / 2 —
`test_io_count`'s 0-second timer did not fire on its pass because the clock had not ticked.
