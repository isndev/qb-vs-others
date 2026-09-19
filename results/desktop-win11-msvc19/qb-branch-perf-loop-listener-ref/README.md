# qb branch `perf/loop-listener-ref` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-199** (`__workflow__` reaches `listener::current` once per
loop instead of three times a pass), measured 2026-09-08 **22:33–22:36 UTC** in one quiet session
(no build during the points, Docker Desktop quit, the WSL2 side idle at 0.17). Control =
`build/ab199-ctl` against `D:\repo\qb-dev\qb` (`develop` `57df433d`), candidate = `build/ab199-cand`
against the worktree `D:\repo\qb-198` at `acf2dd77` (the branch: QB-199, the `messaging-dispatch-batch`
test, the CHANGELOG draft — no dispatch prefetch, QB-198 having been measured and not shipped), both
asserted by SHA and by marker (`listener::current` 5 / 7 occurrences in `VirtualCore.cpp`). Same
flags as the published directories (`/O2 /Ob2 /DNDEBUG`), CPUs 0,2, eight interleaved rounds of
3 repetitions + 1 warm-up per cell, five alternations of 1.5 s per probe.

| file | what |
|---|---|
| `census/` | eight Savina cells × cand/ctl × 8 rounds (JSON) |
| `probe.txt` | `qvoprobe-ask-cost` `push` / `ask` (500 ms timeout), `qvoprobe-pass-cost` k = 1 — cand / ctl × 5 |
| `dispatch-population.txt` | `qvoprobe-dispatch-population`, N = 16 … 16 384, ctl / cand × 3 — the SAME dispatch code on both sides here, so this is the probe's noise on this host, recorded as such |

None of it is merged into the published tables.

## The census (`work_p50 / work_units`, ns, medians of eight interleaved rounds)

| cell | control `57df433d` | **candidate** | Δ | quartiles ctl / cand |
|---|---:|---:|---:|---|
| bank-transaction 1c | 232.2 | 234.7 | +1.1 % | 227.7–233.0 / 230.3–236.6 |
| bank-transaction 2c | 140.2 | 140.2 | level | 138.1–143.4 / 139.1–145.5 |
| big 1c | 16.9 | 16.9 | level | 16.8–17.0 / 16.7–17.0 |
| counting 1c | 10.8 | 9.1 | bimodal on both (8.2–12.0 / 8.5–11.9) | — |
| fib 1c | 178.5 | 176.7 | −1.0 % | 176.6–181.6 / 175.5–178.3 |
| ping-pong 1c | 31.4 | 31.5 | level | 31.1–31.9 / 31.1–31.6 |
| ping-pong 2c | 189.7 | 190.7 | level | 186.7–192.7 / 185.7–194.9 |
| thread-ring 2c | 102.6 | 97.0 | −5.5 %, spreads overlap | 93.1–103.8 / 93.9–99.8 |

## The probes (one core, medians of five)

| probe | control | **candidate** | Δ |
|---|---:|---:|---:|
| `ask` with a 500 ms timeout (ns per trip) | 90.59 | 89.83 | −0.8 % (89.9–91.9 / 89.4–91.8) |
| `push` (ns per trip) | 31.23 | 31.32 | level (31.0–32.1 / 31.2–31.9) |
| `pass-cost` k = 1 (ns per pass) | 15.57 | 15.60 | level |

**Level, as expected**: MSVC initialises thread-local storage at thread start and emits no per-access
init guard, so the three accesses the change removes were three loads here, not three calls into
`__tls_init`; the WSL2 half (`../../wsl-debian-g++14/qb-branch-perf-loop-listener-ref/`) is where the
−1.4 % on ping-pong 1c lives. bank-transaction 1c's +1.1 % has overlapping quartiles and is inside
this host's session-to-session spread for that cell (±2 %).

`dispatch-population` on this host, ctl / cand medians of three (identical code): 6.17 / 6.18 at 16
actors, 5.87 / 5.81 at 64, 6.25 / 6.65 at 256, 6.64 / 6.72 at 512, 7.20 / 7.33 at 1 024,
10.54 / 11.64 at 4 096, 16.61 / 16.78 at 16 384 — the control curve (6.2 → 16.6 ns an event as the
population grows past the caches) is the figure; the ±1–10 % between two identical binaries is what
three 1.5 s points resolve here, and a future A/B on this probe needs more of them.
