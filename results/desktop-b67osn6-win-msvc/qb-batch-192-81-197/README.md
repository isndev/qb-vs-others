# qb `develop` batch QB-192 + QB-81 + QB-197 — Windows 11 / MSVC 19.51

The Windows half of the no-regression measurement for the batch that landed on `develop` on
2026-09-08 (the WSL2 half, with the Savina census, is
`../../wsl-debian-g++14/qb-batch-192-81-197/`): **QB-192** (the wake protocol's cross-thread flags
through the `__atomic` builtins — on MSVC the macros expand to the original access, token for
token, so `ev.c` is the same code here; the qb side adds `listener::_wake_is_pending()`), the four
CI hotfixes, **QB-81** (io_uring at parity on Linux; on Windows only the `iocnt` predicate exists,
a compare per `ev_io_start/stop`) and **QB-197** (the coroutine scheduler owns no loop). Control
`c49868b9` (develop after QB-190/195, the last state measured here) built from
`D:\repo\qb-ctl-batch`, candidate `d20417f9` (develop now) from `D:\repo\qb-dev\qb`, both asserted
by SHA and by marker before the build (`io_is_loop_own` 4 / 0 occurrences, `EV_WAKE_LOAD` 21 / 0,
the scheduler's `loop_` member 0 / 1); each `build.ninja` names its own tree's `ev.c`. Same flags as
the published directories (`/O2 /Ob2 /DNDEBUG`), CPU 0, 1.5 s per point, Docker Desktop quit, the
WSL2 side idle (load 0.00 at both quiet checks). Measured 2026-09-08 **19:41–19:43 UTC** (five
alternations, six probes) and **19:45–19:47 UTC** (ten alternations, the three least noisy probes,
same binaries).

| file | what |
|---|---|
| `probe.txt` | series 1 — `qvoprobe-io-pass` `timer` / `pass` / `wake` (100 µs gaps), `qvoprobe-ask-cost` `push` and `ask` (500 ms timeout), `qvoprobe-pass-cost` k = 1 — cand / ctl × 5 |
| `probe-pass2.txt` | series 2 — `pass-cost` k = 1, `push`, `timer` — ctl / cand × 10 |

None of it is merged into the published tables.

## The probes (one core, medians)

| probe | control `c49868b9` | **candidate `d20417f9`** | Δ | series |
|---|---:|---:|---:|---|
| `pass-cost` k = 1 (ns per pass, no watcher) | 15.91 → 15.88 | 16.09 → **15.86** | +1.1 % → **−0.2 %** | 1 → 2 |
| `push` (ns per trip) | 31.47 → 31.66 | 31.80 → **31.74** | +1.0 % → **+0.3 %** | 1 → 2 |
| busy pass with a far timer (ns) | 30.10 → 30.15 | 30.36 → **30.31** | +0.9 % → **+0.5 %** | 1 → 2 |
| pass with a quiet socket, cold (ns) | 50.69 | 51.30 | +1.2 %, ranges 48.4–51.6 / 48.2–51.8 | 1 |
| `ask` with a 500 ms timeout (ns per trip) | 91.05 | 91.79 | +0.8 %, ranges 90.1–91.7 / 91.5–95.3 | 1 |
| wake latency on that socket, p50 (µs) / ns per pass | 25.6 / 74.4 | 25.1 / 73.0 | level | 1 |

Series 1 read +0.8 to +1.2 % on five of six probes — uniform across probes whose code did not
change on this host (`pass-cost` k = 1 touches nothing the batch touched), every range
overlapping. Series 2, ten alternations on the same binaries two minutes later, has the two
distributions interleaved on all three probes (`pass-cost`: ctl 15.71–16.25, cand 15.65–16.11;
`push`: ctl 31.25–32.29, cand 31.36–32.19; `timer`: ctl 29.73–30.43, cand 29.80–30.59). The
first series was measured right after the two builds; the offset was the session, not the code.
**Level**, as on WSL2 — and as expected: on MSVC the batch changes no instruction on the pass.

Suites at the batch on this host: `verify-windows.ps1 -Presets release` 377 / 377 / 0 with 0
warnings at QB-81's landing; the batch's full gate (five presets, consumer, controls) is run
once for the batch and its figures are in the root commit that bumps the pointers.
