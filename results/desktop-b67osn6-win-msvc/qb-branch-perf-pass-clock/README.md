# qb branch `perf/pass-clock` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-190** (the pass that does not run the loop; qev
`perf/embedder-clock`), measured on 2026-09-08 **14:01–14:03 UTC** in one quiet session (no build
during the points, no leftover probe, Docker Desktop quit), the WSL2 side idle — its own run had
ended at 12:01. Control = `build/ab190-ctl` against `D:\repo\qb-ctl-190` (= `develop` `ae09da3a`),
candidate = `build/ab190-cand` against `D:\repo\qb-dev\qb` (the branch's working tree, final code,
the branch's qev — which on this host also carries the QB-195 fix: the loop's monotonic clock is
`QueryPerformanceCounter` for the first time, see below). Same flags as the published directories
(`/O2 /Ob2 /DNDEBUG`), CPU 0, candidate and control alternated five times, 1.5 s per point.

| file | what |
|---|---|
| `probe.txt` | `qvoprobe-io-pass` `timer` (a busy core holding one far libev timer, no socket), `pass` (a quiet loopback socket, cold between two polls) and `wake` (the latency of a byte on that socket, 100 µs gaps), `qvoprobe-ask-cost` `push` and `ask` with a 500 ms timeout, `qvoprobe-pass-cost` k = 1 — cand / ctl × 5. |

None of it is merged into the published tables.

## The probes (one core, medians of five)

| probe | control (`ae09da3a`) | **branch** | Δ |
|---|---:|---:|---:|
| **busy pass with a far timer (ns)** | 47.7 | **29.5** | **−38 %** |
| **pass with a quiet socket, cold (ns)** | 82.9 | **49.5** | **−40 %** (both bimodal: ctl 72.9–84.6, cand 41.7–54.7) |
| wake latency on that socket, p50 (µs) | 19.2 | 19.7 | level (bimodal ~9 / ~20 on both, the host's) |
| push (ns per trip) | 30.96 | 31.40 | inside the spread (ctl 30.86–32.13, cand 30.89–31.55) |
| ask with a 500 ms timeout (ns per trip) | 89.9 | 89.0 | level |
| `pass-cost` k = 1 (ns per pass, no watcher) | 15.6 | 15.9 | level |

What a busy core paid for one far timer here: **47.7 − 15.6 = 32 ns** a pass, now **14**; the gate
is the same as on g++ (`rdtsc`, the count, `ev_timer_next`, the estimate).

## Two things this host taught

**The gate had to go out of line.** With it written inline in `listener::run()` — which every
core's pass inlines — `push` read **31.0 → 33.1 ns** (+6.5 %, five alternations, fully separated)
on a core whose pass never enters the gate: two actors exchanging events, no timer, no socket,
`has_work()` false. Neither the loop's struct growing (`now_set` moved to its end: unchanged) nor
the listener's (its new fields moved last: unchanged; the control's `listener.h` plus 48 bytes of
padding: +0.5) explained it; the gate's double arithmetic out of line (`_timer_due` marked
`QB_NOINLINE`) did — `push` 32.0, the timed ask level — and the whole gate out of line
(`_nowait_gate`) closes it: `push` 31.2 against 31.2. What the compiler did to the pass around an
inlined gate it never takes cost more than the call the gate now is (a few loads, on the passes
that take it). g++ showed nothing of it either way.

**The `QueryPerformanceCounter` clock of QB-193 had never been compiled in (Huly QB-195).** The
first `ev_now_set` case failed here alone: `ev_clock_now()` read Unix seconds. libev's "fixes any
misconfiguration" block forces `EV_USE_MONOTONIC` to 0 wherever `CLOCK_MONOTONIC` is undefined —
MSVC defines it nowhere — 450 lines after QB-193 had set it to 1, so the loop's "monotonic" time
was the precise system time, stepped by every wall-clock adjustment, and the QPC path was dead
code in qev and in qb's copy alike. Exempted now: `bench-pass` `timer` 31.5 → 28.6 ns (QPC is the
cheaper read), `timer+set0` 15.0, `gate` 0.8, `now` 16.8; qev's MSVC loop suite 58 / 0 / 3, wepoll
13 / 0 / 0.
