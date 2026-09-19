# macbook-m4pro-macos-clang21

Apple M4 Pro (10 performance + 4 efficiency cores, 48 GB, High Power mode, on AC), macOS 26.6.2
(25G83), AppleClang 21.0.0 (clang-2100.1.1.101), `-O3 -DNDEBUG`, **9 repetitions + 2 warmup**,
every one of the **132 cells** across the eight `savina-*/` directories measured in one quiet
session on **2026-09-19, 12:17:56–12:25:21 UTC** — 130 verified + 2 declared `n/a`
(`caf-detached` has no spin mode), one build, one manifest (`run.json` carries no
`merged_partial_runs`). **qb in `savina-*/` is the 3.2.0 candidate, `develop` `174e515a`** — as on
the two pinned hosts since 2026-09-13, where the column is `f2779605` — built with CAF 1.1.0,
SObjectizer 5.8.5.1 and the raw-thread floor under `build/macos-candidate`; shipped v3.1.0
(`830ea244`) was measured minutes earlier in the same session and lives in
`qb-branch-develop/grid-shipped-3.1.0-20260919/`. `tools/check-roster.py --results
results/macbook-m4pro-macos-clang21` is clean: the 48 cells of fib, chameneos and bank-transaction
this host had never measured are here. The session's protocol, its controls, its censuses and what
it found are in **`qb-branch-develop/README.md`**; the field of 2026-09-05 (five shapes, 7 + 2,
shipped 3.1.0 as the qb column) is in git history, and what that session taught stays below.

**This host is UNPINNED, and every document says so.** macOS has no CPU affinity API a program
can read back — `qb::CPU::ThreadPinningSupported()` is false, and the harness refuses `--cpus` on
a platform where a pin could report success and do nothing (FAIRNESS.md 1.4). Every cell was run
with `--no-pin`; every result document carries `pinned:false`, and qb's own caveat line ("THIS
PLATFORM HAS NO REAL THREAD PINNING") is in each of its documents. What that costs, measured: the
two-core cells are bimodal by launch on this host (the scheduler decides which core pair and
when), so **no two-core figure is quoted from the grid alone** — the launch censuses under
`qb-branch-develop/` are the instrument for those cells, exactly as on Windows (§9.11). One-core
cells are steady (min–max spread 1–4 %).

`REPORT.md` beside this file is `tools/report.py`'s render of this directory and
`tools/check-report.py` fails if it drifts. `docs/TUNING.md` §13.9 is the reading guide for the
2026-09-19 session, §9.13 for everything under `qb-branch-perf-event-pipe-segmented/`.

| directory | what it is |
|---|---|
| `savina-ping-pong/` | **20 cells** — 18 verified + 2 declared `n/a` (`caf-detached` has no spin mode); 2026-09-19, qb = the candidate `174e515a`. |
| `savina-counting/`, `savina-thread-ring/`, `savina-fork-join/`, `savina-big/`, `savina-fib/`, `savina-chameneos/`, `savina-bank-transaction/` | **16 cells** each, all verified, same session; `caf-detached` declares itself omitted from these seven. |
| `qb-branch-develop/` | **the 3.2.0 candidate on this host** (2026-09-19): `grid-174e515a/` and its second pass, `grid-shipped-3.1.0-20260919/` and `grid-f2779605-20260919/` (the two controls), `census-174e515a-field/` and `census-174e515a-vs-controls/` (the two-core cells, 12 interleaved launches), `bisect-f2779605-174e515a/` (the two one-core cells that moved the other way, attributed). Its README carries the tables. |
| `qb-branch-perf-event-pipe-segmented/grid-final/` | **the candidate of 2026-09-05**: qb at `perf/event-pipe-segmented` `279e6cd4` through the same adapters, all five benchmarks, 7 + 2, 19:06 UTC. |
| `qb-branch-perf-event-pipe-segmented/grid-230c5035/` | the previous candidate (`perf/core-hot-path` `230c5035`, before the segmented pipe), same protocol, 19:07 UTC — the control for what the two `event-pipe-segmented` commits change. |
| `qb-branch-perf-event-pipe-segmented/grid-shipped-3.1.0/` | shipped v3.1.0 through the same protocol, 19:07–19:08 UTC — the control for the whole branch. |
| `qb-branch-perf-event-pipe-segmented/burst-sweep/` | `savina/counting`, one core, spin, the burst swept 2 k → 4 M messages for the candidate, `230c5035` and 3.1.0 **interleaved per burst** (`tools/burst-sweep.py`), 7 + 2, with the process's page reclaims from `/usr/bin/time -l` beside each document (`*.faults.txt`); CAF, SObjectizer and the floor at 30 k and 1 M. The §9.11 instrument on its third host. |
| `qb-branch-perf-event-pipe-segmented/launch-census/` | the candidate against `230c5035` on the six bimodal two-core cells (ping-pong, thread-ring, big × spin/park): **12 launches interleaved, 3 + 1 each** (`tools/launch-census.py`). The figure to quote for those cells. |
| `qb-branch-perf-event-pipe-segmented/launch-census-pingpong-2cpark-24x5/` | the third instrument on the one residual (ping-pong, two cores, park): 24 launches, 5 + 1. |
| `qb-branch-perf-event-pipe-segmented/axis-k/round{1,2,3}-{with,without}/` | the axis-K A/B on arm64 — qb at `6a0897c0` (`Mailbox::notify()` fences in spin mode too, a `dmb ish` here) against its parent `61b0b4cf`, ping-pong and thread-ring, 2c-spin and 2c-park, three interleaved rounds of 7 + 2. |
| `qb-branch-perf-event-pipe-segmented/axis-k/launch-census/` | the same A/B as a 12-launch census — the figure to quote. |

**What the 2026-09-05 session said, in one paragraph each** (all in §9.13 with the tables; its field tables are in git history, its branch directories are still here):

- **Same-core dispatch**: candidate ping-pong 1c-spin **49.7 ns** against `230c5035` 53.8 and
  3.1.0 84.9 (−7.6 % / −41 %); counting 6.6 / 8.3 / 11.6; fork-join 6.2 / 8.6 / 12.9; thread-ring
  25.9 / 26.8 / 42.0; big 15.0 / 19.6 / 23.9. Where MSVC kept a 1–2 % one-core loss against
  `230c5035`, clang/arm64 gains 7.6 %.
- **The burst sweep has no cliff to remove on macOS**: `230c5035` reads 6.9 → 8.6 ns/msg from 2 k
  to 4 M and 3.1.0 9.3 → 11.3, where the same binaries climbed to 40 and 43 on WSL2 — XNU's
  zero-fill fault is cheap and the copy ladder does not re-fault. The candidate is flat at
  **5.6–6.2** and the fastest at every burst. Page reclaims at 1 M: **4 344** against 8 364 /
  8 365 — halved, not divided by 800 as on Linux, because `MADV_POPULATE_WRITE` does not exist on
  Darwin and a 2 MB slab is still faulted one 16 KB page at a time on first touch.
- **Two-core cells, by census**: ping-pong 2c-spin 156.3 vs 187.4 (−16.6 %), big 2c-spin 16.3 vs
  17.6 and 2c-park 15.3 vs 17.3 (−7 to −11 %), thread-ring level (+1.6 / +4.9 %) — every pair of
  distributions overlaps. **One residual, three instruments agreeing on the sign**: ping-pong
  2c-park **210.6 vs 186.9** (12 launches, +12.7 %), 209.7 vs 187.5 (24 × 5, +11.8 %), grid 209.6
  vs 185.4; distributions overlap (candidate 185–235, control 166–241). Recorded, not explained.
- **Axis K on arm64: no measurable effect.** with/without census: ping-pong 2c-spin 190.4 vs 181.5
  (+4.9 %), 2c-park 199.1 vs 190.5 (+4.5 %); thread-ring 2c-spin 79.8 vs 89.1 (−10.5 %), 2c-park
  99.6 vs 88.7 (+12.3 %) — mixed signs, every pair overlapping. Spin stays faster than park in every
  candidate cell (ping-pong 152.7 vs 209.6, thread-ring 76.9 vs 93.2), so the `dmb ish` did not
  invert the two modes.
- **The park floor on this host is the condition variable**: `baseline__2c-park` ping-pong 4.62 µs,
  shipped qb 6.85 µs, thread-ring floor 2.49 µs / qb 3.39 µs. The branch's park handshake (axes
  A/B/C, `61b0b4cf`) takes qb's 2c-park ping-pong to ~210 ns — 30× — which is the single largest
  move of the session and the reason the `2c-park` column of `savina-*/` must be read with the
  README's caveat.

**Residual load during the 2026-09-05 session**, since it is part of the measurement: a `Virtualization.framework`
VM (~33 % of one core, constant), two `pnpm dev` / `tsx watch` dev servers (idle), and macOS's
`StorageManagement` service scanning after 20 GB of fresh build output (60–110 % of a core,
decaying). 1-minute load average 4.5–5.9 at the start of the bench phase on 14 logical cores. The
other AI agents on the machine were paused before the first measurement.
