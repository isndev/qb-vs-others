# qb `develop` batch QB-192 + QB-81 + QB-197 — WSL2 Debian 13 / g++ 14.2

The no-regression census for the batch that landed on `develop` on 2026-09-08 (the full gate runs
per batch, the release measurement per step — the user's "packer les devs"): **QB-192** (the wake
protocol's cross-thread flags through the `__atomic` builtins, the embedded `qev` target taking the
sanitizer and coverage flags so `ev.c` is instrumented at last), the four CI hotfixes that
instrumenting `ev.c` surfaced (the clang function-sanitizer exemption on the two callback-dispatch
functions, atomic gcov counters, a consumed `read()` in the shared fd fixture, `ev_wrap.h` in the
generator's order), **QB-81** (io_uring from 47× slower than epoll to parity: the deadline timerfd
armed only for a poll that sleeps, `TASKRUN_FLAG`, the backend's timerfd out of `iocnt`; epoll stays
the default) and **QB-197** (the coroutine scheduler owns no loop, an awaiter's loop is required — no
global default loop created from a core thread). Control `ee34bb13` (develop after QB-190/195, the
last state censused), candidate `57df433d` (develop now); the qb copy embeds its `qev` tree, so the
qb SHA fixes the loop (`7eb4678` → `5fe159b`). Measured 2026-09-08 **19:28–19:30 UTC** in one quiet
window (no build during the points, the Windows side idle), `~/qvo-ctl-batch` and `~/qvo-cand-batch`
built from `git archive` of those SHAs against this harness at `45babe1`, same flags (`-O3 -DNDEBUG`),
CPUs 0,2, candidate and control interleaved ten rounds, 3 repetitions + 1 warm-up per point.

| file | what |
|---|---|
| `census/` | the four Savina cells × ctl/cand × 10 rounds (JSON), the two build logs, `census-summary.txt` |
| `probe.txt` | `qvoprobe-io-pass` `pass` (a quiet loopback socket on the default epoll, 1 µs cadence) and `timer` (a busy core holding one far timer), `qvoprobe-ask-cost` `push` and `ask` — five alternations, 2 s each, CPU 2; `probe-summary.txt` the medians |

None of it is merged into the published tables.

## The census (`work_p50 / work_units`, ns, medians of ten interleaved rounds)

| cell | control `ee34bb13` | **candidate `57df433d`** | Δ |
|---|---:|---:|---:|
| bank-transaction 2c park (the ask/coroutine cell) | 81.8 (79.3–83.4) | 82.8 (80.3–85.9) | +1.2 %, spreads overlap |
| ping-pong 1c park | 22.8 (22.5–23.0) | 22.7 (22.5–23.1) | level |
| ping-pong 2c park | 157.0 (154.1–161.6) | 154.7 (148.4–159.9) | −1.5 %, spreads overlap |
| thread-ring 2c park | 74.5 (73.2–75.9) | 75.1 (73.7–79.4) | +0.8 %, spreads overlap |

## The probes (one core, medians of five)

| probe | control | **candidate** | Δ |
|---|---:|---:|---:|
| `ask` (ns per trip) | 46.8 (46.3–47.0) | 46.2 (46.0–47.4) | −1.2 %, inside the spread |
| `push` (ns per trip) | 24.0 (23.8–24.6) | 23.8 (23.7–24.3) | inside the spread |
| pass with a quiet socket, epoll, 1 µs cadence (ns) | 27.9 (27.8–28.4) | 28.0 (27.8–28.4) | level |
| busy pass with a far timer (ns) | 25.7 (25.6–26.2) | 25.7 (25.6–26.1) | level |

No regression, and none was expected from the numbers already in hand: the QB-192 atomics compile
to the same `movl` as the volatile accesses before them (disassembly, and the MSVC preprocessor
expands them to the original access); the QB-81 changes live in `ev_iouring.c`, which the default
epoll backend never runs, plus the `iocnt` predicate that costs a compare per `ev_io_start/stop`;
QB-197 removes a dead member. The io_uring figures themselves (25.8 ns on the quiet-socket pass
against epoll's 28.3, 40.9 against 126.9 when polled on every pass, timer-only at parity, wake p50
4.18 against 3.98 µs, park p50 41.3 against 41.6 µs) are in Huly QB-81 and in
`qb/readme/6_guides/performance_tuning.md` ("io_uring, measured (3.2.0)").

Suites at the batch: the whole qb suite forced onto io_uring 385/385 under `sanitize` and
`sanitize-thread` with the ten `-ev-*` matrix variants, the superproject presets green with `ev.c`
instrumented (19 `__tsan` / 25 `__asan` hooks in `ev.c.o`, gcov), qev standalone Release and TSan
green, Windows release 377/377 with 0 warnings; every CI lane of qb, qev and the superproject green
on the landed SHAs.
