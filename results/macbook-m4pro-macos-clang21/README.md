# macbook-m4pro-macos-clang21

Apple M4 Pro (10 performance + 4 efficiency cores, 48 GB), macOS 26.6 (25G72), AppleClang 21.0.0
(Xcode 26), `-O3 -DNDEBUG`, **7 repetitions + 2 warmup**, every one of the **84 cells** across the
five `savina-*/` directories measured in one quiet session on 2026-09-05 (18:59–19:07 UTC), the
candidate qb branch's grids, burst sweep and launch censuses following at 19:06–19:21 UTC in the
same session. Nothing else of ours ran meanwhile: the other agents on this machine were paused
first, and the residual load is recorded below. `run.json` carries `merged_partial_runs: 1`
because the field was assembled from two builds of the same tree (see the second paragraph).
**`savina-fib/` and `savina-chameneos/` are not measured on this host yet** (they were written on
2026-09-06, after this session): `tools/check-roster.py --results results/macbook-m4pro-macos-clang21`
reports the 32 missing cells by name until the next macOS session fills them, and the shipped
3.1.0 fib cell there will carry the logging cost the two pinned hosts' READMEs describe.

**This host is UNPINNED, and every document says so.** macOS has no CPU affinity API a program
can read back — `qb::CPU::ThreadPinningSupported()` is false, and the harness refuses `--cpus` on
a platform where a pin could report success and do nothing (FAIRNESS.md 1.4). Every cell was run
with `--no-pin`; every result document carries `pinned:false`, and qb's own caveat line ("THIS
PLATFORM HAS NO REAL THREAD PINNING") is in each of its documents. What that costs, measured: the
two-core cells of ping-pong, thread-ring and big are bimodal within a launch on this host (the
scheduler decides which core pair and when), so **no two-core figure below is quoted from the
grid alone** — the launch census (`launch-census*/`) is the instrument for those cells, exactly as
on Windows (§9.11). One-core cells are steady (min–max spread 1–4 %).

qb in `savina-*/` is the **shipped v3.1.0** (`830ea244`), built under `build/macos-shipped` with
`QVO_WITH_CAF=OFF QVO_WITH_SOBJECTIZER=OFF`; CAF 1.1.0, SObjectizer 5.8.5.1 and the floor come from
`build/macos-candidate` — they do not depend on qb, and the two builds share the compiler and the
flag set. Both were merged into this directory by `run.py --only`, which refuses to merge unless
host, platform, CPU set, repetitions and warmup agree.

`REPORT.md` beside this file is `tools/report.py`'s render of this directory and
`tools/check-report.py` fails if it drifts. `docs/TUNING.md` §9.13 is the reading guide for
everything under `qb-branch-perf-event-pipe-segmented/`.

| directory | what it is |
|---|---|
| `savina-ping-pong/` | **20 cells** — 18 verified + 2 declared `n/a` (`caf-detached` has no spin mode). |
| `savina-counting/`, `savina-thread-ring/`, `savina-fork-join/`, `savina-big/` | **16 cells** each, all verified; `caf-detached` declares itself omitted from these four. |
| `qb-branch-perf-event-pipe-segmented/grid-final/` | **the candidate**: qb at `perf/event-pipe-segmented` `279e6cd4` through the same adapters, all five benchmarks, 7 + 2, 19:06 UTC. |
| `qb-branch-perf-event-pipe-segmented/grid-230c5035/` | the previous candidate (`perf/core-hot-path` `230c5035`, before the segmented pipe), same protocol, 19:07 UTC — the control for what the two `event-pipe-segmented` commits change. |
| `qb-branch-perf-event-pipe-segmented/grid-shipped-3.1.0/` | shipped v3.1.0 through the same protocol, 19:07–19:08 UTC — the control for the whole branch. |
| `qb-branch-perf-event-pipe-segmented/burst-sweep/` | `savina/counting`, one core, spin, the burst swept 2 k → 4 M messages for the candidate, `230c5035` and 3.1.0 **interleaved per burst** (`tools/burst-sweep.py`), 7 + 2, with the process's page reclaims from `/usr/bin/time -l` beside each document (`*.faults.txt`); CAF, SObjectizer and the floor at 30 k and 1 M. The §9.11 instrument on its third host. |
| `qb-branch-perf-event-pipe-segmented/launch-census/` | the candidate against `230c5035` on the six bimodal two-core cells (ping-pong, thread-ring, big × spin/park): **12 launches interleaved, 3 + 1 each** (`tools/launch-census.py`). The figure to quote for those cells. |
| `qb-branch-perf-event-pipe-segmented/launch-census-pingpong-2cpark-24x5/` | the third instrument on the one residual (ping-pong, two cores, park): 24 launches, 5 + 1. |
| `qb-branch-perf-event-pipe-segmented/axis-k/round{1,2,3}-{with,without}/` | the axis-K A/B on arm64 — qb at `6a0897c0` (`Mailbox::notify()` fences in spin mode too, a `dmb ish` here) against its parent `61b0b4cf`, ping-pong and thread-ring, 2c-spin and 2c-park, three interleaved rounds of 7 + 2. |
| `qb-branch-perf-event-pipe-segmented/axis-k/launch-census/` | the same A/B as a 12-launch census — the figure to quote. |

**What the session said, in one paragraph each** (all in §9.13 with the tables):

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

**Residual load during the session**, since it is part of the measurement: a `Virtualization.framework`
VM (~33 % of one core, constant), two `pnpm dev` / `tsx watch` dev servers (idle), and macOS's
`StorageManagement` service scanning after 20 GB of fresh build output (60–110 % of a core,
decaying). 1-minute load average 4.5–5.9 at the start of the bench phase on 14 logical cores. The
other AI agents on the machine were paused before the first measurement.
