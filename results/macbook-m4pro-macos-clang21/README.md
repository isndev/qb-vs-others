# macbook-m4pro-macos-clang21

Apple M4 Pro (10 performance + 4 efficiency cores, 48 GB, on AC), macOS 26.6.2 (25G83), AppleClang
21.0.0 (clang-2100.3.34.2), CMake 4.4.3, `-O3 -DNDEBUG`, **9 repetitions + 2 warmup**, **unpinned**.
**The whole Savina suite — thirty shapes, 484 cells — with shipped qb 3.3.0 as the qb column**, measured
in one quiet session on **2026-10-27, 04:54:23–05:21:46 UTC** (480 verified + 2 declared `n/a`,
`caf-detached` has no spin mode; the uct and sieve rows re-measured in a second session the same
morning, 06:21:49–06:32:08 UTC, see below), one manifest (`run.json`, `merged_partial_runs` 1 for that
second session). qb is **v3.3.0 as released** (`e18022dd`, frozen by `git archive`), CAF 1.1.0,
SObjectizer 5.8.5.1 and the raw-thread floor, all from the harness tree `13169b7d` (the
`qb-3.3.0-field` tag) under `build-shipped`; the 3.4.0 branch tip qb `3a771d3b` was measured beside it
in the same session and lives in `qb-branch-develop/grid-3a771d3b/`. The twenty-two shapes of Savina
waves A, B and C are measured here for the first time; the eight shapes this host measured on
2026-09-19 with the 3.2.0 candidate `174e515a` are replaced — that field is in git history (below).
`tools/check-roster.py --results results/macbook-m4pro-macos-clang21` is clean on the thirty shapes.
The session's protocol, its builds, its controls, its censuses and what it found are in
**`qb-branch-develop/README.md`** and `docs/TUNING.md` §22.

**This host is UNPINNED, and every document says so.** macOS has no CPU affinity API a program can
read back — `qb::CPU::ThreadPinningSupported()` is false, and the harness refuses `--cpus` on a
platform where a pin could report success and do nothing (FAIRNESS.md 1.4). Every cell was run with
`--no-pin`; every result document carries `pinned:false`, and qb's own caveat line ("THIS PLATFORM HAS
NO REAL THREAD PINNING") is in each of its documents. A two-core cell is one launch, bimodal by launch
on this host (the scheduler decides which core pair and when), so **no two-core figure is quoted from
the grid alone** — the launch censuses under `qb-branch-develop/` are the instrument for those cells:
the candidate against shipped 3.3.0 on every shape, and the field (qb / CAF / SObjectizer / floor) on
the twelve B/C shapes. One-core cells are steadier, not steady — read them by their distribution, as
`REPORT.md` prints it.

`REPORT.md` beside this file is `tools/report.py`'s render of this directory and `tools/check-report.py`
fails if it drifts.

| directory | what it is |
|---|---|
| `savina-ping-pong/` | **20 cells** — 18 verified + 2 declared `n/a` (`caf-detached` has no spin mode); 2026-10-27, qb = shipped 3.3.0. |
| the other 29 `savina-<shape>/` | **16 cells** each, all verified, same session (`uct`, `sieve`: the second session); `caf-detached` declares itself omitted from them. |
| `wave-bc-form-sweep/` | the forms the adapters declare, measured as side documents (`report.py` never renders them): sieve `place=1` (qb, floor) and `local=1` (all four), apsp `form=1`, uct `nodes=200000` (all four), concdict and concsll `form=1` (qb, CAF), logmap's SObjectizer individual binding — 58 documents, `docs/TUNING.md` §22.4. |
| `qb-branch-develop/` | **the 3.4.0 branch tip on this host** (2026-10-27): `grid-3a771d3b/` and `grid-3a771d3b-pass2/` (qb `3a771d3b`, the five commits over v3.3.0, before and after the field), `census-3a771d3b-vs-3.3.0/` (every shape, two cores, 12 interleaved launches of 3 + 1), `census-3.3.0-field-bc/` (the field's two-core cells of the twelve B/C shapes, four binaries in a seeded order), `bisect-1c/` (the one-core cells the grid read slower: three physical copies per side over three builds). Its README carries the tables; the earlier cohorts (`grid-174e515a/` and its controls and censuses, 2026-09-19) stay beside them. |

**In git history only.** The field of 2026-09-19 (eight shapes, 132 cells, qb = the 3.2.0 candidate
`174e515a`, read in `docs/TUNING.md` §13.9) was replaced by this session's documents; every one of its
files exists at the parent of the commit that replaced them, 13169b7d:
`git show 13169b7d:results/macbook-m4pro-macos-clang21/savina-fib/qb__1c-spin.json` reads one, and
`git archive 13169b7d results/macbook-m4pro-macos-clang21 | tar -x` restores the directory as it
was. The field of 2026-09-05 (five shapes, 7 + 2, shipped 3.1.0) is at `30838d50`, as before:

| directory at `30838d50` | what it is |
|---|---|
| `qb-branch-perf-event-pipe-segmented/grid-final/` | **the candidate of 2026-09-05**: qb at `perf/event-pipe-segmented` `279e6cd4` through the same adapters, all five benchmarks, 7 + 2, 19:06 UTC. |
| `qb-branch-perf-event-pipe-segmented/grid-230c5035/` | the previous candidate (`perf/core-hot-path` `230c5035`, before the segmented pipe), same protocol, 19:07 UTC — the control for what the two `event-pipe-segmented` commits change. |
| `qb-branch-perf-event-pipe-segmented/grid-shipped-3.1.0/` | shipped v3.1.0 through the same protocol, 19:07–19:08 UTC — the control for the whole branch. |
| `qb-branch-perf-event-pipe-segmented/burst-sweep/` | `savina/counting`, one core, spin, the burst swept 2 k → 4 M messages for the candidate, `230c5035` and 3.1.0 **interleaved per burst** (`tools/burst-sweep.py`), 7 + 2, with the process's page reclaims from `/usr/bin/time -l` beside each document (`*.faults.txt`); CAF, SObjectizer and the floor at 30 k and 1 M. The §9.11 instrument on its third host. |
| `qb-branch-perf-event-pipe-segmented/launch-census/` | the candidate against `230c5035` on the six bimodal two-core cells (ping-pong, thread-ring, big × spin/park): **12 launches interleaved, 3 + 1 each** (`tools/launch-census.py`). |
| `qb-branch-perf-event-pipe-segmented/launch-census-pingpong-2cpark-24x5/` | the third instrument on the one residual (ping-pong, two cores, park): 24 launches, 5 + 1. |
| `qb-branch-perf-event-pipe-segmented/axis-k/round{1,2,3}-{with,without}/` | the axis-K A/B on arm64 — qb at `6a0897c0` (`Mailbox::notify()` fences in spin mode too, a `dmb ish` here) against its parent `61b0b4cf`, ping-pong and thread-ring, 2c-spin and 2c-park, three interleaved rounds of 7 + 2. |
| `qb-branch-perf-event-pipe-segmented/axis-k/launch-census/` | the same A/B as a 12-launch census — the figure to quote. |

What the 2026-09-05 and 2026-09-19 sessions said stays in `docs/TUNING.md` §9.13 and §13.9; the
2026-10-27 session is §22.

**Residual load during the 2026-10-27 session**, since it is part of the measurement — measured
before and after every leg by a guard that waited for 60 s (then 30 s) of quiet before each
(`qb-branch-develop/grid-3a771d3b/session.log`): nothing of ours ran on the host (no build, no suite,
no benchmark but the leg's; the other agents' processes idle; the Linux guest of
`../utm-debian13-arm64-g++14/` powered off). What did run: the window server and the desktop apps of
the maintainer's session (`WindowServer`, the Claude and Codex desktop apps, Chrome) — 2–2.5 of the 14
logical cores busy at the first guard, 0.7–1.0 from the field on; Docker Desktop's guest (one idle
registry container) 0–14 % of a core; `duetexpertd` 72 % of a core for the seconds before leg A and
nothing after. CPU idle over the ten seconds before each leg: 86.4 % (A), 95.0 % (C, the field),
93.6 % (A2), 94.0 % (D), 94.7 % (E1), 94.4 % (E2); 1-minute load 2.96 before A, 1.4–2.6 from then on.
