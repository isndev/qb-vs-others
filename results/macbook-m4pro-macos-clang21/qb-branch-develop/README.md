# qb on macOS / Apple M4 Pro / AppleClang 21 — the branch tips measured against what shipped

Two cohorts live here. **The first section is the 3.4.0 branch tip `3a771d3b` against shipped 3.3.0
(2026-10-27)**; the second, from "qb `develop` `174e515a`" on, is the 3.2.0 candidate against shipped
3.1.0 and `f2779605` (2026-09-19), kept as measured.

## qb `3a771d3b` — the 3.4.0 branch tip against shipped 3.3.0, thirty shapes (2026-10-27)

qb `3a771d3b` is the tip of the shared integration branch of the 3.4.0 programme: five commits over
v3.3.0 — two timing tests (QB-1150, QB-1141), a benchmark (QB-248), **QB-1114** (`c16f1fa9`, `Actor.cpp`:
the ask registry constant-initialised, its warm path inline) and **QB-1151** (`3a771d3b`,
`system/allocator/pipe.h` + `io/pipe.cpp`: the character pipe formats integers in place). It was measured
through the unmodified adapters of qb-vs-others `13169b7d` (plus the two caveat amendments of the second
session) on **all thirty Savina shapes**, against **shipped v3.3.0** (`e18022dd`, frozen by `git archive`,
768 blobs checked against the tag) in ONE quiet unpinned session on **2026-10-27, 04:48–06:41 UTC** —
Apple M4 Pro (10 performance + 4 efficiency cores, 48 GB, on AC), macOS 26.6.2 (25G83), AppleClang 21.0.0
(clang-2100.3.34.2), CMake 4.4.3, `-O3 -DNDEBUG`, 9 repetitions + 2 warmup, `--no-pin` (FAIRNESS.md 1.4:
every document `pinned:false`, no two-core figure quoted from a grid alone). Three builds from the same
frozen tree: `build-shipped` (qb v3.3.0, CAF 1.1.0, SObjectizer 5.8.5.1, the floor, the probes; 121
binaries, 0 warnings) — the field of `../savina-*/`; `build-cand` (qb `3a771d3b`, qb only, 30 binaries, 0
warnings); `build-1114` (qb `c16f1fa9`, qb only, for the bisection). `tools/negative-control.py` on the
field build before the session and after the second session's relink: **151 CAUGHT / 43 CONFIRMED / 0
MISSED** both times, 194 identities. The legs, their times, the host at each guard and the two things the
session had to redo are in `docs/TUNING.md` §22.1; `grid-3a771d3b/session.log` is the session's log.

| directory | when (UTC) | what |
|---|---|---|
| `grid-3a771d3b/` | 04:48:28–04:53:29 (uct, sieve: 06:23:00–06:23:03) | **the branch tip**, `--only qb`, 120 cells, 120 / 120 verified — README.md's macOS `framework=qb` grid; `session.log`, `run-leg-A.log`. |
| `../savina-<shape>/` (the field) | 04:54:23–05:21:46 (uct, sieve rows: 06:21:49–06:22:06) | **484 cells, every framework**, qb = shipped 3.3.0, 0 unverified, 2 declared `n/a` — a fresh `../run.json` (`merged_partial_runs` 1: the second session's two rows); the same-session control of the branch tip, and the published field. |
| `grid-3a771d3b-pass2/` | 05:22:41–05:27:35 (uct, sieve: 06:23:44–06:23:45) | the branch tip again, after the field, 120 / 120 verified — the per-cell repeatability column below. |
| `../wave-bc-form-sweep/` | 05:28:29–05:39:04 (uct, sieve: 06:24:26–06:30:07) | leg D, the declared forms on the shipped build: 58 side documents, `docs/TUNING.md` §22.4. |
| `census-3a771d3b-vs-3.3.0/<shape>/` | 05:41:56–06:03:00 (uct, sieve: 06:30:48–06:30:55) | **the two-core comparison**: 12 interleaved launches of 3 + 1 in AB/BA order, branch tip against shipped, every shape, 2c-spin and 2c-park — 1 440 launches, 0 unverified; `census.log` per shape. |
| `census-3.3.0-field-bc/<shape>/` | 06:03:55–06:20:08 (uct, sieve: 06:31:36–06:32:08) | **the field census on the twelve B/C shapes**: qb / CAF / SObjectizer / floor of the shipped build, 12 launches of 3 + 1 in one seeded order per configuration, two cores — 1 152 launches, 0 unverified; the figure to quote for those cells. |
| `bisect-1c/<shape>/` | 06:33:50–06:41:02 | **the one-core bisection**: shipped / `c16f1fa9` / `3a771d3b`, three physical copies per side, nine shapes, 12 launches of 3 + 1 in a seeded order, 1c-spin and 1c-park — 1 944 launches, 0 unverified. |
| `bisect-1c-original/<shape>/`, `bisect-1c-original/repetitions/` | 06:48:02–06:48:06, then 27-repetition runs | each side's original file against its copy on a-star, fib and bank-transaction (96 launches a shape); and 3 / 9 / 27 repetitions in one process, three runs per build on a-star — the reading of the grid's a-star and fib cells. |
| `profiles-3.3.0-bc/` | 06:41:17, 06:45:30–06:47:17 | the first `sample` profile of each B/C shape and of concsll, qb at 1c-spin, summarised per shape (§22.5). |
| `bisect-1c-variants/` | 09:00–10:3x | **QB-1154, attributed and fixed**: the same three-copies census on bank-transaction over the variants that located the cost — `align-census/` (both sides at `-falign-functions=64`), `pad-census/` (256 and 768 bytes of never-executed code before the registry), `v1-census/` (3.3.0's TLS initialisation around the 1114 registry), `v4-census/` and `v4-census2/` (the 1114 directories under the 3.3.0 control shape: one inline `owner_queue`; one and two cores, and bitonicsort), `final-census/` (the fix as committed, qb `b67ac7b8`); each directory's `summary.txt` is `bisect-summary.py`'s reading, `pmu-cpu-profile-shipped-vs-c16f1fa9.txt` the Instruments CPU Profiler aggregation by leaf symbol and instruction, `*-time-*.txt` the `/usr/bin/time -l` instruction and cycle counts. `docs/TUNING.md` §22.3. |

**What it says** (§22.3 has the reading): at two cores the branch tip is level on every shape (−1.9 to
+2.6 % by census, every pair overlapping); at one core **bank-transaction is +10.7 / +11.3 % per transfer**,
and the bisection puts it whole in `c16f1fa9` (+12.9 / +14.4 %, CI +11.7..+15.5, three copies a side
agreeing) with `3a771d3b` giving two points back — Huly QB-1154; bitonicsort +1.9 % in the same commit;
a-star's +8–14 % and fib's +5 % of the grids are the host's slow one-core mode, visited by every build
(`bisect-1c-original/`), not the candidate.

## Shipped 3.3.0 and the branch tip, same session (ns per unit, p50)

The field columns are `../savina-<shape>/` of this same session; `~` marks a pair whose [min, p99]
ranges overlap (`~A` / `~A2`: one of the two passes); the census column is the two-core instrument
(median of twelve launch medians, branch tip vs shipped, `~` when the twelve-launch ranges overlap).

| shape (per unit) | config | shipped 3.3.0 | **`3a771d3b`** | pass 2 | Δ A / A2 vs 3.3.0 | census: cand vs 3.3.0 | min: 3.3.0 / cand / pass 2 | CAF / SObjectizer / floor |
|---|---|---|---|---|---|---|---|---|
| `a-star` (node) | 1c-spin | 265.4 | **287.0** | 303.7 | +8.1 % / +14.4 % | — | 262.9 / 285.7 / 293.9 | 269.4 / 266.7 / 250.0 |
|  | 1c-park | 265.2 | **298.4** | 304.8 | +12.5 % / +14.9 % ~A | — | 263.7 / 259.3 / 285.1 | 270.1 / 268.5 / 275.1 |
|  | 2c-spin | 163.3 | **173.4** | 163.9 | +6.2 % / +0.4 % ~A2 | 163.7 vs 163.8 (-0.1 % ~) | 162.8 / 171.6 / 163.3 | 149.8 / 158.2 / 151.3 |
|  | 2c-park | 164.7 | **156.9** | 163.0 | -4.7 % / -1.1 % ~ | 163.6 vs 163.5 (+0.0 % ~) | 162.7 / 151.6 / 155.7 | 150.1 / 162.3 / 154.3 |
| `apsp` (block phase) | 1c-spin | 572.7 | **625.1** | 573.3 | +9.1 % / +0.1 % ~ | — | 569.3 / 540.9 / 541.7 | 1,523.6 / 1,137.2 / 550.1 |
|  | 1c-park | 572.4 | **587.6** | 569.2 | +2.7 % / -0.6 % ~ | — | 570.7 / 530.5 / 564.3 | 1,531.7 / 1,185.5 / 556.1 |
|  | 2c-spin | 289.4 | **297.1** | 301.5 | +2.7 % / +4.2 % ~ | 303.5 vs 299.0 (+1.5 % ~) | 288.0 / 292.3 / 291.1 | 1,586.5 / 1,426.2 / 300.3 |
|  | 2c-park | 290.6 | **297.7** | 295.5 | +2.4 % / +1.7 % ~ | 303.5 vs 299.1 (+1.5 % ~) | 286.8 / 290.4 / 289.2 | 1,591.8 / 1,412.4 / 298.2 |
| `bank-transaction` (transfer) | 1c-spin | 83.2 | **89.8** | 94.4 | +8.0 % / +13.4 % | — | 82.0 / 87.5 / 88.7 | 485.9 / 346.5 / 29.6 |
|  | 1c-park | 82.0 | **92.7** | 90.8 | +13.0 % / +10.7 % | — | 81.0 / 89.0 / 86.5 | 525.2 / 356.9 / 31.6 |
|  | 2c-spin | 62.2 | **63.7** | 65.3 | +2.5 % / +5.0 % ~ | 64.6 vs 64.1 (+0.8 % ~) | 60.8 / 62.8 / 60.3 | 644.3 / 640.3 / 85.5 |
|  | 2c-park | 63.0 | **64.0** | 66.7 | +1.5 % / +5.9 % ~ | 66.4 vs 65.7 (+1.0 % ~) | 62.3 / 61.4 / 62.5 | 660.0 / 621.5 / 86.6 |
| `barber` (haircut) | 1c-spin | 3,262.8 | **3,322.3** | 3,247.4 | +1.8 % / -0.5 % ~A2 | — | 3,250.0 / 3,309.1 / 3,239.6 | 4,453.8 / 5,095.1 / 3,463.5 |
|  | 1c-park | 3,253.4 | **3,317.9** | 3,460.3 | +2.0 % / +6.4 % ~ | — | 3,245.4 / 3,286.3 / 3,253.0 | 4,514.3 / 4,779.0 / 2,990.4 |
|  | 2c-spin | 2,150.6 | **2,317.5** | 2,167.1 | +7.8 % / +0.8 % ~ | 2,162.0 vs 2,174.9 (-0.6 % ~) | 2,140.8 / 2,030.9 / 2,153.4 | 4,429.9 / 4,567.4 / 1,506.3 |
|  | 2c-park | 2,153.3 | **2,317.5** | 2,168.4 | +7.6 % / +0.7 % ~A2 | 2,171.7 vs 2,161.5 (+0.5 % ~) | 2,127.0 / 2,276.7 / 2,157.5 | 4,291.5 / 4,471.7 / 1,515.2 |
| `big` (round trip) | 1c-spin | 12.1 | **12.2** | 12.1 | +0.9 % / +0.3 % ~ | — | 11.9 / 12.0 / 12.0 | 254.6 / 139.5 / 11.5 |
|  | 1c-park | 12.0 | **12.0** | 12.1 | -0.1 % / +0.8 % ~ | — | 11.5 / 11.6 / 12.0 | 250.8 / 146.9 / 12.2 |
|  | 2c-spin | 12.6 | **13.2** | 13.6 | +4.2 % / +7.9 % ~ | 13.0 vs 12.8 (+1.6 % ~) | 12.5 / 12.3 / 12.8 | 170.3 / 355.9 / 30.5 |
|  | 2c-park | 12.8 | **13.1** | 13.8 | +2.9 % / +8.4 % ~ | 12.7 vs 12.8 (-0.1 % ~) | 12.5 / 12.8 / 12.9 | 377.6 / 361.1 / 30.8 |
| `bitonicsort` (message) | 1c-spin | 6.7 | **6.9** | 6.8 | +3.3 % / +1.7 % ~A2 | — | 6.6 / 6.8 / 6.7 | 90.5 / 56.7 / 7.1 |
|  | 1c-park | 6.7 | **6.8** | 6.8 | +1.6 % / +1.0 % ~ | — | 6.7 / 6.8 / 6.7 | 91.2 / 58.9 / 7.2 |
|  | 2c-spin | 7.0 | **7.3** | 7.3 | +4.1 % / +4.2 % ~A | 7.0 vs 6.9 (+1.3 % ~) | 6.8 / 6.8 / 7.1 | 59.8 / 112.5 / 7.4 |
|  | 2c-park | 6.7 | **7.0** | 6.9 | +3.6 % / +1.9 % ~A2 | 6.9 vs 6.8 (+2.2 % ~) | 6.7 / 6.9 / 6.7 | 59.6 / 119.9 / 7.1 |
| `bndbuffer` (item) | 1c-spin | 15,499.5 | **15,837.4** | 15,417.2 | +2.2 % / -0.5 % ~ | — | 15,356.6 / 15,717.7 / 15,325.7 | 14,953.9 / 14,796.7 / 14,426.5 |
|  | 1c-park | 15,616.4 | **15,806.3** | 15,339.3 | +1.2 % / -1.8 % ~ | — | 15,350.0 / 15,732.9 / 15,321.8 | 15,076.6 / 14,745.3 / 14,650.7 |
|  | 2c-spin | 8,895.5 | **9,271.1** | 8,889.6 | +4.2 % / -0.1 % ~ | 8,947.0 vs 8,938.5 (+0.1 % ~) | 8,308.9 / 8,903.7 / 8,451.1 | 7,932.4 / 7,944.6 / 13,654.2 |
|  | 2c-park | 8,742.4 | **9,357.9** | 8,944.9 | +7.0 % / +2.3 % ~ | 8,947.7 vs 8,932.2 (+0.2 % ~) | 8,326.8 / 8,563.6 / 8,629.9 | 7,923.0 / 8,168.0 / 13,575.6 |
| `chameneos` (meeting) | 1c-spin | 23.7 | **23.5** | 23.4 | -1.0 % / -1.5 % ~ | — | 23.4 / 23.0 / 23.1 | 548.7 / 273.0 / 64.3 |
|  | 1c-park | 23.7 | **23.9** | 23.1 | +0.8 % / -2.2 % ~ | — | 23.2 / 23.5 / 22.8 | 527.4 / 283.7 / 65.6 |
|  | 2c-spin | 36.9 | **30.6** | 38.1 | -17.3 % / +3.0 % ~A2 | 37.3 vs 36.8 (+1.5 % ~) | 36.5 / 28.7 / 37.2 | 920.6 / 978.0 / 164.0 |
|  | 2c-park | 36.6 | **39.5** | 37.2 | +7.9 % / +1.5 % ~ | 37.7 vs 36.8 (+2.6 % ~) | 35.8 / 37.3 / 35.9 | 931.4 / 671.8 / 109.8 |
| `cigsmok` (round) | 1c-spin | 1,583.1 | **1,620.5** | 1,581.1 | +2.4 % / -0.1 % ~ | — | 1,580.6 / 1,616.1 / 1,576.8 | 1,736.9 / 1,620.2 / 1,437.7 |
|  | 1c-park | 1,650.4 | **1,617.2** | 1,579.1 | -2.0 % / -4.3 % ~ | — | 1,582.7 / 1,612.9 / 1,575.4 | 1,736.6 / 1,608.5 / 1,453.0 |
|  | 2c-spin | 1,477.6 | **1,574.7** | 1,484.5 | +6.6 % / +0.5 % ~A2 | 1,486.7 vs 1,485.5 (+0.1 % ~) | 1,430.7 / 1,569.4 / 1,417.9 | 1,240.7 / 1,011.3 / 1,312.1 |
|  | 2c-park | 1,486.0 | **1,569.6** | 1,487.0 | +5.6 % / +0.1 % ~ | 1,485.8 vs 1,487.5 (-0.1 % ~) | 1,471.8 / 1,439.6 / 1,444.3 | 1,242.6 / 1,266.6 / 3,914.8 |
| `concdict` (operation) | 1c-spin | 21.5 | **41.3** | 21.7 | +92.3 % / +0.8 % ~ | — | 20.9 / 22.6 / 21.0 | 293.9 / 92.6 / 23.5 |
|  | 1c-park | 21.6 | **21.6** | 21.2 | +0.0 % / -1.5 % ~ | — | 21.1 / 20.8 / 21.0 | 276.2 / 101.3 / 24.7 |
|  | 2c-spin | 29.5 | **31.2** | 29.4 | +5.7 % / -0.4 % ~ | 29.6 vs 29.8 (-0.7 % ~) | 28.8 / 28.7 / 28.8 | 583.7 / 422.3 / 55.0 |
|  | 2c-park | 28.3 | **30.6** | 47.5 | +8.2 % / +67.6 % ~A | 30.8 vs 30.8 (-0.1 % ~) | 27.7 / 28.1 / 32.5 | 617.9 / 358.6 / 268.4 |
| `concsll` (request) | 1c-spin | 20,753.7 | **21,426.7** | 20,918.7 | +3.2 % / +0.8 % ~ | — | 20,319.9 / 20,871.5 / 20,354.4 | 19,217.0 / 18,575.9 / 18,968.5 |
|  | 1c-park | 20,764.9 | **21,418.4** | 20,757.7 | +3.1 % / -0.0 % ~ | — | 20,574.8 / 20,831.6 / 20,517.7 | 19,238.9 / 18,600.1 / 18,990.3 |
|  | 2c-spin | 21,966.0 | **23,122.3** | 22,094.1 | +5.3 % / +0.6 % ~ | 22,025.0 vs 21,965.2 (+0.3 % ~) | 21,814.8 / 22,593.5 / 21,758.8 | 20,944.3 / 20,454.8 / 20,536.8 |
|  | 2c-park | 22,734.4 | **23,510.1** | 22,845.7 | +3.4 % / +0.5 % ~ | 22,708.9 vs 22,735.1 (-0.1 % ~) | 22,650.6 / 22,954.4 / 22,708.6 | 21,332.2 / 22,681.1 / 22,258.9 |
| `counting` (message) | 1c-spin | 3.8 | **4.0** | 3.8 | +3.8 % / -0.3 % ~ | — | 3.8 / 3.8 / 3.8 | 72.8 / 58.4 / 4.3 |
|  | 1c-park | 3.8 | **3.9** | 3.8 | +1.1 % / -0.6 % ~ | — | 3.8 / 3.8 / 3.8 | 71.6 / 61.4 / 4.4 |
|  | 2c-spin | 5.4 | **5.5** | 5.3 | +2.9 % / -2.2 % ~ | 5.2 vs 5.2 (+0.1 % ~) | 5.3 / 5.3 / 5.1 | 122.8 / 217.5 / 51.2 |
|  | 2c-park | 5.3 | **5.4** | 5.1 | +2.8 % / -2.8 % ~ | 5.2 vs 5.2 (+0.6 % ~) | 5.1 / 5.2 / 5.1 | 120.3 / 148.6 / 43.6 |
| `facloc` (customer) | 1c-spin | 75.7 | **76.9** | 76.6 | +1.5 % / +1.1 % ~ | — | 75.4 / 76.0 / 75.5 | 696.2 / 500.6 / 70.7 |
|  | 1c-park | 75.8 | **76.8** | 76.0 | +1.3 % / +0.2 % ~ | — | 75.3 / 76.5 / 75.4 | 666.9 / 536.6 / 72.9 |
|  | 2c-spin | 85.2 | **83.1** | 85.3 | -2.5 % / +0.2 % ~ | 82.2 vs 82.6 (-0.5 % ~) | 83.7 / 81.7 / 81.7 | 938.5 / 1,529.7 / 71.9 |
|  | 2c-park | 84.0 | **83.2** | 82.9 | -1.0 % / -1.3 % ~ | 83.1 vs 83.3 (-0.2 % ~) | 82.2 / 82.6 / 82.6 | 424.5 / 1,519.3 / 71.8 |
| `fib` (actor) | 1c-spin | 61.0 | **64.1** | 64.6 | +5.2 % / +5.9 % | — | 60.5 / 63.1 / 62.7 | 874.7 / 864.8 / 47.2 |
|  | 1c-park | 60.8 | **63.4** | 64.1 | +4.4 % / +5.4 % ~A | — | 60.5 / 61.7 / 63.3 | 868.7 / 892.5 / 48.2 |
|  | 2c-spin | 38.8 | **40.9** | 40.5 | +5.6 % / +4.4 % ~ | 40.1 vs 39.8 (+0.7 % ~) | 37.3 / 38.2 / 38.9 | 824.0 / 2,332.9 / 33.3 |
|  | 2c-park | 38.6 | **41.4** | 40.6 | +7.1 % / +5.2 % ~ | 40.1 vs 39.9 (+0.5 % ~) | 37.3 / 35.2 / 38.3 | 800.2 / 2,321.8 / 55.0 |
| `filterbank` (sample) | 1c-spin | 45,554.2 | **45,799.0** | 45,489.4 | +0.5 % / -0.1 % ~ | — | 44,491.2 / 44,109.5 / 44,818.4 | 45,401.8 / 43,751.6 / 40,939.8 |
|  | 1c-park | 45,341.1 | **45,177.7** | 45,241.8 | -0.4 % / -0.2 % ~ | — | 44,010.3 / 44,697.7 / 44,450.0 | 45,517.3 / 43,463.4 / 40,993.5 |
|  | 2c-spin | 24,079.4 | **25,341.3** | 24,117.0 | +5.2 % / +0.2 % ~ | 24,008.6 vs 24,102.7 (-0.4 % ~) | 23,767.3 / 23,870.5 / 23,773.4 | 23,300.8 / 27,280.8 / 21,133.8 |
|  | 2c-park | 24,048.7 | **25,245.1** | 23,958.0 | +5.0 % / -0.4 % ~A2 | 23,998.4 vs 24,073.3 (-0.3 % ~) | 23,686.6 / 24,853.4 / 23,495.4 | 23,410.6 / 26,845.8 / 21,149.7 |
| `fork-join` (message) | 1c-spin | 4.8 | **5.0** | 4.8 | +2.4 % / -0.3 % ~ | — | 4.3 / 4.3 / 4.3 | 129.5 / 46.8 / 19.8 |
|  | 1c-park | 5.0 | **4.9** | 4.8 | -2.3 % / -3.5 % ~ | — | 4.3 / 4.3 / 4.3 | 133.4 / 50.1 / 20.0 |
|  | 2c-spin | 5.3 | **5.4** | 5.3 | +1.5 % / -0.5 % ~ | 5.2 vs 5.3 (-1.2 % ~) | 5.0 / 5.1 / 5.0 | 114.3 / 265.7 / 23.2 |
|  | 2c-park | 5.0 | **5.2** | 5.2 | +4.0 % / +5.5 % ~ | 5.4 vs 5.4 (-0.4 % ~) | 4.9 / 5.0 / 5.0 | 118.1 / 203.1 / 22.0 |
| `fork-join-create` (actor) | 1c-spin | 54.8 | **54.6** | 54.6 | -0.2 % / -0.3 % ~ | — | 54.0 / 52.6 / 53.9 | 674.7 / 463.5 / 30.2 |
|  | 1c-park | 55.1 | **54.6** | 54.2 | -0.9 % / -1.6 % ~ | — | 53.7 / 52.6 / 53.4 | 691.6 / 484.7 / 28.5 |
|  | 2c-spin | 29.5 | **28.2** | 27.2 | -4.4 % / -7.8 % ~ | 28.5 vs 28.7 (-0.8 % ~) | 27.1 / 26.8 / 26.2 | 689.3 / 1,562.5 / 27.3 |
|  | 2c-park | 29.6 | **28.8** | 26.9 | -2.7 % / -9.2 % ~ | 28.5 vs 29.0 (-1.7 % ~) | 27.2 / 27.7 / 26.2 | 772.1 / 1,546.1 / 28.7 |
| `logmap` (term) | 1c-spin | 16.5 | **16.7** | 16.7 | +1.2 % / +1.0 % ~ | — | 16.4 / 16.5 / 16.1 | 347.8 / 102.1 / 34.2 |
|  | 1c-park | 16.5 | **16.7** | 16.4 | +1.1 % / -0.6 % ~A2 | — | 16.4 / 16.6 / 16.3 | 348.9 / 117.9 / 35.2 |
|  | 2c-spin | 13.7 | **14.1** | 13.9 | +2.9 % / +1.1 % ~ | 12.6 vs 12.6 (+0.1 % ~) | 12.0 / 12.9 / 12.4 | 522.1 / 192.4 / 21.5 |
|  | 2c-park | 13.5 | **13.7** | 13.8 | +1.9 % / +2.4 % ~ | 12.5 vs 12.5 (+0.2 % ~) | 12.4 / 12.2 / 12.6 | 554.5 / 190.7 / 30.9 |
| `nqueens` (solution) | 1c-spin | 10,672.4 | **10,804.8** | 10,690.6 | +1.2 % / +0.2 % ~A2 | — | 10,604.8 / 10,754.0 / 10,601.6 | 10,522.3 / 10,432.9 / 10,231.5 |
|  | 1c-park | 10,809.4 | **10,845.2** | 10,646.4 | +0.3 % / -1.5 % ~ | — | 10,639.6 / 10,809.1 / 10,565.6 | 10,517.5 / 10,556.7 / 10,204.4 |
|  | 2c-spin | 5,406.9 | **5,492.2** | 5,354.3 | +1.6 % / -1.0 % ~ | 5,366.1 vs 5,382.4 (-0.3 % ~) | 5,373.4 / 5,363.7 / 5,339.7 | 5,471.9 / 5,560.7 / 5,284.0 |
|  | 2c-park | 5,412.2 | **5,413.4** | 5,435.8 | +0.0 % / +0.4 % ~ | 5,435.6 vs 5,976.1 (-9.0 % ~) | 5,354.2 / 5,360.1 / 5,372.0 | 5,451.0 / 5,550.2 / 5,288.9 |
| `philosophers` (meal) | 1c-spin | 47.5 | **48.2** | 47.9 | +1.3 % / +0.7 % ~ | — | 47.2 / 46.5 / 46.3 | 424.1 / 379.7 / 46.2 |
|  | 1c-park | 47.2 | **48.7** | 47.0 | +3.2 % / -0.4 % ~A2 | — | 46.6 / 47.7 / 45.7 | 396.7 / 418.9 / 47.1 |
|  | 2c-spin | 102.9 | **98.9** | 95.5 | -3.9 % / -7.1 % ~ | 95.5 vs 95.6 (-0.1 % ~) | 93.9 / 95.6 / 92.5 | 1,060.8 / 963.8 / 182.5 |
|  | 2c-park | 99.0 | **96.0** | 95.2 | -3.0 % / -3.7 % ~ | 96.5 vs 95.3 (+1.3 % ~) | 91.3 / 93.9 / 94.2 | 1,098.0 / 830.7 / 594.5 |
| `ping-pong` (round trip) | 1c-spin | 22.5 | **22.9** | 22.9 | +1.9 % / +1.8 % ~ | — | 22.3 / 22.6 / 22.4 | 251.4 / 125.0 / 3.7 |
|  | 1c-park | 23.0 | **23.6** | 22.8 | +2.5 % / -0.7 % ~A2 | — | 22.4 / 23.4 / 22.1 | 256.1 / 146.7 / 3.7 |
|  | 2c-spin | 125.8 | **134.3** | 126.5 | +6.7 % / +0.6 % ~A2 | 119.0 vs 119.2 (-0.2 % ~) | 122.9 / 129.4 / 124.1 | 422.3 / 578.1 / 155.6 |
|  | 2c-park | 125.7 | **134.6** | 125.9 | +7.1 % / +0.1 % ~ | 119.4 vs 119.5 (-0.0 % ~) | 123.2 / 122.0 / 124.8 | 431.8 / 5,582.1 / 4,310.5 |
| `piprecision` (term) | 1c-spin | 8,231.6 | **8,433.8** | 8,220.4 | +2.5 % / -0.1 % ~A2 | — | 8,219.5 / 8,398.1 / 8,201.9 | 7,566.4 / 7,955.6 / 8,402.1 |
|  | 1c-park | 8,242.5 | **8,440.6** | 8,218.8 | +2.4 % / -0.3 % ~ | — | 8,225.8 / 8,414.9 / 8,201.7 | 7,558.8 / 7,941.1 / 7,724.3 |
|  | 2c-spin | 4,853.5 | **4,914.7** | 4,963.8 | +1.3 % / +2.3 % ~ | 4,963.7 vs 4,963.9 (-0.0 % ~) | 4,594.2 / 4,679.5 / 4,842.0 | 4,551.2 / 4,813.2 / 8,305.4 |
|  | 2c-park | 4,960.6 | **5,248.1** | 4,960.5 | +5.8 % / -0.0 % ~A2 | 4,956.3 vs 4,954.4 (+0.0 % ~) | 4,808.5 / 5,221.5 / 4,947.2 | 4,056.1 / 4,308.0 / 7,386.5 |
| `quicksort` (value) | 1c-spin | 42.0 | **42.8** | 42.2 | +1.8 % / +0.3 % ~ | — | 41.6 / 42.4 / 42.0 | 41.7 / 44.3 / 40.3 |
|  | 1c-park | 41.9 | **42.4** | 41.9 | +1.1 % / -0.0 % ~ | — | 41.5 / 42.0 / 41.8 | 42.1 / 44.6 / 40.5 |
|  | 2c-spin | 26.0 | **25.9** | 25.9 | -0.5 % / -0.5 % ~ | 25.2 vs 25.1 (+0.4 % ~) | 25.4 / 24.3 / 25.2 | 23.0 / 26.5 / 24.1 |
|  | 2c-park | 25.7 | **26.9** | 25.3 | +4.7 % / -1.4 % ~ | 25.1 vs 25.2 (-0.3 % ~) | 24.9 / 25.7 / 25.0 | 22.8 / 26.7 / 23.6 |
| `radixsort` (message) | 1c-spin | 5.8 | **5.9** | 5.8 | +1.9 % / -0.1 % ~ | — | 5.6 / 5.7 / 5.7 | 73.0 / 28.6 / 10.1 |
|  | 1c-park | 5.8 | **5.9** | 5.8 | +1.4 % / +0.4 % ~ | — | 5.7 / 5.8 / 5.7 | 73.9 / 32.3 / 10.3 |
|  | 2c-spin | 6.0 | **6.1** | 6.0 | +2.1 % / -0.1 % ~ | 5.8 vs 5.8 (-0.1 % ~) | 5.8 / 5.8 / 5.8 | 77.3 / 101.8 / 10.9 |
|  | 2c-park | 5.8 | **5.8** | 5.8 | +1.3 % / +0.7 % ~ | 5.8 vs 5.8 (+0.0 % ~) | 5.8 / 5.8 / 5.8 | 76.5 / 101.8 / 11.3 |
| `recmatmul` (leaf) | 1c-spin | 1,148,845.4 | **1,161,236.0** | 1,147,458.2 | +1.1 % / -0.1 % ~ | — | 1,144,184.0 / 1,154,290.7 / 1,142,935.3 | 1,096,117.7 / 1,085,750.5 / 1,086,411.5 |
|  | 1c-park | 1,145,972.4 | **1,166,148.1** | 1,149,547.8 | +1.8 % / +0.3 % ~A2 | — | 1,141,797.4 / 1,152,777.3 / 1,143,178.3 | 1,090,911.0 / 1,087,703.7 / 1,084,767.2 |
|  | 2c-spin | 671,214.7 | **674,842.2** | 671,037.2 | +0.5 % / -0.0 % ~ | 670,661.9 vs 670,241.9 (+0.1 % ~) | 669,837.8 / 671,182.9 / 665,648.2 | 592,690.3 / 550,722.7 / 653,688.5 |
|  | 2c-park | 677,121.3 | **680,615.4** | 675,651.0 | +0.5 % / -0.2 % ~ | 673,148.2 vs 674,259.4 (-0.2 % ~) | 674,238.0 / 676,541.7 / 674,401.4 | 553,470.1 / 559,810.9 / 652,800.6 |
| `sieve` (candidate) | 1c-spin | 507.8 | **508.5** | 506.8 | +0.1 % / -0.2 % ~ | — | 501.5 / 490.0 / 490.2 | 618.4 / 562.1 / 492.2 |
|  | 1c-park | 505.8 | **515.1** | 502.4 | +1.8 % / -0.7 % ~ | — | 503.0 / 502.5 / 493.8 | 618.6 / 566.0 / 493.7 |
|  | 2c-spin | 506.0 | **528.0** | 526.1 | +4.3 % / +4.0 % ~ | 509.6 vs 508.3 (+0.3 % ~) | 504.1 / 507.6 / 508.7 | 382.4 / 424.5 / 498.5 |
|  | 2c-park | 522.8 | **504.8** | 520.7 | -3.5 % / -0.4 % ~ | 510.3 vs 508.4 (+0.4 % ~) | 504.5 / 502.7 / 503.0 | 388.8 / 437.8 / 491.6 |
| `sor` (update) | 1c-spin | 35.9 | **36.2** | 36.2 | +0.8 % / +0.8 % ~ | — | 35.5 / 35.8 / 35.6 | 462.4 / 291.9 / 34.7 |
|  | 1c-park | 36.0 | **36.4** | 37.2 | +1.1 % / +3.4 % ~ | — | 35.7 / 35.9 / 37.0 | 467.4 / 295.1 / 35.9 |
|  | 2c-spin | 24.0 | **18.5** | 18.8 | -22.9 % / -21.9 % ~ | 18.6 vs 18.5 (+0.7 % ~) | 18.5 / 18.2 / 18.3 | 701.4 / 587.6 / 20.1 |
|  | 2c-park | 24.1 | **18.6** | 18.7 | -22.8 % / -22.6 % ~ | 18.7 vs 18.6 (+0.4 % ~) | 18.5 / 18.2 / 18.5 | 702.5 / 595.3 / 20.5 |
| `thread-ring` (hop) | 1c-spin | 11.4 | **11.6** | 11.6 | +1.8 % / +1.6 % ~ | — | 11.2 / 11.3 / 11.4 | 119.3 / 55.4 / 3.4 |
|  | 1c-park | 11.3 | **11.5** | 11.5 | +2.1 % / +2.1 % ~A2 | — | 11.0 / 11.4 / 11.4 | 131.5 / 63.8 / 3.5 |
|  | 2c-spin | 60.2 | **62.1** | 64.5 | +3.0 % / +7.0 % ~ | 56.7 vs 57.3 (-1.0 % ~) | 55.7 / 58.4 / 59.4 | 198.3 / 341.8 / 87.1 |
|  | 2c-park | 60.7 | **64.8** | 60.5 | +6.7 % / -0.4 % ~ | 57.9 vs 57.5 (+0.8 % ~) | 57.3 / 61.0 / 58.9 | 202.1 / 195.4 / 2,460.1 |
| `trapezoid` (trapezoid) | 1c-spin | 22.6 | **23.0** | 22.6 | +1.6 % / +0.0 % ~A2 | — | 22.5 / 22.8 / 22.5 | 21.5 / 21.5 / 21.5 |
|  | 1c-park | 22.5 | **22.9** | 22.6 | +1.9 % / +0.4 % ~A2 | — | 22.5 / 22.8 / 22.5 | 21.5 / 21.5 / 21.5 |
|  | 2c-spin | 11.1 | **11.2** | 11.1 | +0.5 % / +0.1 % ~ | 11.1 vs 11.1 (+0.0 % ~) | 11.1 / 11.2 / 11.1 | 11.0 / 10.9 / 10.9 |
|  | 2c-park | 11.2 | **11.2** | 11.2 | -0.1 % / -0.1 % ~ | 11.1 vs 11.1 (-0.1 % ~) | 11.1 / 11.2 / 11.1 | 11.0 / 10.9 / 10.9 |
| `uct` (node) | 1c-spin | 128.6 | **135.6** | 131.6 | +5.4 % / +2.3 % ~ | — | 127.0 / 129.4 / 128.7 | 1,206.0 / 1,264.4 / 58.7 |
|  | 1c-park | 129.0 | **140.3** | 129.5 | +8.8 % / +0.4 % ~A2 | — | 127.5 / 133.9 / 127.0 | 1,237.9 / 1,289.9 / 57.7 |
|  | 2c-spin | 91.8 | **96.5** | 99.3 | +5.1 % / +8.2 % ~A | 95.2 vs 93.4 (+1.9 % ~) | 90.1 / 93.5 / 94.9 | 1,158.7 / 1,583.9 / 62.5 |
|  | 2c-park | 90.8 | **98.5** | 97.0 | +8.5 % / +6.8 % ~A2 | 95.0 vs 93.1 (+2.1 % ~) | 88.3 / 96.3 / 93.8 | 1,131.2 / 1,584.2 / 60.3 |

## The two-core cells, by the field census (median of the 12 launch medians, ns per unit, spin / park)

| shape | qb (shipped 3.3.0) | CAF | SObjectizer | floor |
|---|---:|---:|---:|---:|
| radixsort | **5.8 / 5.8** | 74.1 / 72.7 | 99.3 / 104.8 | 10.8 / 11.3 |
| filterbank | 23 650 / 23 748 | 24 357 / 24 243 | 28 013 / 27 827 | **21 963 / 21 988** |
| sieve | 507.6 / 506.6 | **392.4 / 395.3** | 427.8 / 439.0 | 497.2 / 490.2 |
| trapezoid | 11.1 / 11.1 | 11.0 / 11.0 | **10.9 / 10.9** | 10.9 / 10.9 |
| piprecision | 4 964 / 4 956 | **4 152 / 4 093** | 4 420 / 4 406 | 8 303 / 7 755 |
| recmatmul | 670 328 / 674 293 | 589 955 / 587 969 | **550 126 / 551 478** | 654 753 / 652 086 |
| quicksort | 25.0 / 25.0 | **23.2 / 23.1** | 24.7 / 24.8 | 24.2 / 24.1 |
| bitonicsort | **6.9 / 6.8** | 64.1 / 61.2 | 115.9 / 119.6 | 7.3 / 7.4 |
| apsp | **290.2 / 289.6** | 1 588 / 1 584 | 1 448 / 1 433 | 298.1 / 298.4 |
| sor | **18.5 / 18.6** | 704.1 / 710.9 | 585.9 / 587.1 | 20.1 / 20.6 |
| uct | 93.7 / 91.1 | 1 211 / 1 201 | 1 561 / 1 655 | **64.4 / 61.4** |
| facloc | 83.1 / 82.1 | 826.7 / 903.2 | 1 551 / 1 559 | **70.8 / 73.3** |

## The one-core bisection (ns per unit, median of 36 launch medians over three copies; bootstrap 95 % CI of the ratio)

| cell | shipped 3.3.0 | `c16f1fa9` (QB-1114) | `3a771d3b` (+ QB-1151) |
|---|---:|---:|---:|
| bank-transaction 1c-spin | 81.55 | **92.05 (+12.9 %, +11.7..+14.3, separate)** | 90.23 (+10.7 %, +9.6..+11.8, separate) |
| bank-transaction 1c-park | 81.44 | **93.18 (+14.4 %, +12.6..+15.5, separate)** | 90.63 (+11.3 %, +10.4..+12.1, separate) |
| bitonicsort 1c-spin / 1c-park | 6.70 / 6.69 | 6.82 / 6.81 (+1.9 / +1.8 %, separate) | 6.82 / 6.82 (+1.9 / +1.9 %, separate) |
| thread-ring 1c-spin / 1c-park | 11.42 / 11.42 | 11.46 / 11.46 (+0.4 / +0.3 %, overlapping) | 11.48 / 11.52 (+0.5 / +0.8 %, CI touching 0) |
| a-star 1c-spin / 1c-park | 264.9 / 265.2 | 265.6 / 265.2 | 264.7 / 264.5 |
| fib 1c-spin / 1c-park | 61.03 / 61.11 | 61.18 / 61.23 | 61.22 / 61.14 |
| piprecision 1c-spin / 1c-park | 8 221 / 8 222 | 8 225 / 8 223 | 8 219 / 8 222 |
| sieve 1c-spin / 1c-park | 508.5 / 509.0 | 508.9 / 509.4 | 508.8 / 508.8 |
| trapezoid 1c-spin / 1c-park | 22.59 / 22.61 | 22.59 / 22.60 | 22.60 / 22.59 |
| ping-pong 1c-spin / 1c-park | 22.74 / 22.86 | 22.85 / 23.00 | 22.94 / 22.91 |

The copies' A/A spread (the three medians of one side) is 0.0–0.8 % everywhere but bank-transaction's
shipped side at 1c-spin (3.2 %); every CI above is of the ratio of pooled medians.

## Residual load, since it is part of the measurement

Measured by the guard before and after every leg (`grid-3a771d3b/session.log`), never assumed: nothing
of ours ran beside a leg — no build, no suite, no other benchmark; the other agents' processes idle;
the Linux guest powered off. What did run: the window server and the desktop apps of the maintainer's
session (`WindowServer`, the Claude and Codex desktop apps, Chrome) — 2–2.5 of the 14 logical cores
busy at the first guard, 0.7–1.0 from the field on; Docker Desktop's guest (one idle registry
container) 0–14 % of a core; `duetexpertd` 72 % of a core in the seconds before leg A and never after.
CPU idle over the ten seconds before each leg: 86.4 % (A), 95.0 % (C), 93.6 % (A2), 94.0 % (D), 94.7 %
(E1), 94.4 % (E2), 94.7 % (C2), 93.7 % (the bisection); 1-minute load 2.96 before A, 1.4–2.6 after.

---

## qb `develop` `174e515a` — the 3.2.0 candidate on macOS / Apple M4 Pro / AppleClang 21 (2026-09-19)

The macOS third of the **3.2.0 candidate grid** (the two others are
`../../desktop-win11-msvc19/qb-branch-develop/` and `../../wsl-debian-g++14/qb-branch-develop/`),
and the first time this host measures all **eight** Savina shapes: qb `develop` at **`174e515a`** —
the release candidate of 2026-09-13 (`f2779605`) plus the four changes that landed after it, the
actor arena (QB-212), `pin_frame_copy` (QB-213), the frame-free `qb::ask` (QB-214) and
`qb::growable_ring` (QB-215) — through the unmodified adapters, against shipped v3.1.0 (`830ea244`)
AND against `f2779605`, all in ONE quiet session on **2026-09-19, 12:13:44–12:40:30 UTC**. Apple M4 Pro
(10 performance + 4 efficiency cores, 48 GB, High Power mode, on AC), macOS 26.6.2 (25G83), AppleClang
21.0.0 (clang-2100.1.1.101), CMake 4.4.3, `-O3 -DNDEBUG`, **9 repetitions + 2 warmup**, **unpinned**
(`--no-pin`: macOS has no affinity API a program can read back, FAIRNESS.md 1.4 — every document
carries `pinned:false`, and no two-core figure below is quoted from a grid alone). Three builds, made
before the quiet window (11:55:14–11:57:30 UTC, the harness rebuilt from nothing each time):
`build/macos-candidate` is the full field — qb at this superproject's submodule, CAF 1.1.0,
SObjectizer 5.8.5.1, the raw-thread floor, the probes — 33 binaries, 0 warnings;
`build/macos-shipped` is the v3.1.0 tree (`git archive v3.1.0`, `QB_FRAMEWORK_VERSION "3.1.0"`,
frameworks and probes off); `build/macos-rc0913` is `git archive f2779605`, probes on.
`tools/negative-control.py` on the candidate build: CAUGHT=7 CONFIRMED=4 MISSED=0;
`tools/guards-negative-control.py`: CAUGHT=33 CONFIRMED=3 MISSED=0. Then 120 s of quiet, qb's own
`dev/bench` gate (three runs, 12:00:48–12:13:44 UTC — VERDICT PASS against the 3.0-era baseline, 0
regressed, 8 improved), and the session below. The whole tree had passed its macOS validation
immediately before (ten presets, sanitizers, corpus, guards and their controls — Huly QB-44).

| directory | when (UTC) | what |
|---|---|---|
| `grid-174e515a/` | 12:13:44–12:14:16 | **the candidate**, 32 cells, 32 / 32 verified — README.md's macOS `framework=qb` grid. |
| `grid-shipped-3.1.0-20260919/` | 12:14:16–12:17:31 | **the control**, v3.1.0 through the same adapters, 32 / 32 verified. Its fib cells carry the logging cost the two pinned hosts' READMEs describe (nine `LOG_INFO` lines per actor lifetime at 3.1.0's default): ~2.0–2.2 µs per actor here. |
| `grid-f2779605-20260919/` | 12:17:31–12:17:46 | the release candidate of 2026-09-13, the control for the four changes that landed after it, 32 / 32 verified. |
| `grid-174e515a-pass2/` | 12:17:46–12:17:56 | the candidate again, after the two controls — 32 / 32 verified. |
| `../savina-*/` (the field itself) | 12:17:56–12:25:21 | **132 cells, every framework** (qb at the candidate, CAF, CAF-detached, SObjectizer, the floor), 9 + 2, 0 unverified, 2 declared `n/a` — a fresh `../run.json`; the 2026-09-05 field (five shapes, 7 + 2, shipped qb) is in git history. |
| `census-174e515a-field/` | 12:25:21–12:27:02 | 12 interleaved launches of 3 + 1 on ping-pong and thread-ring, 2c-spin (qb / CAF / floor) and 2c-park (qb / CAF): the cells that decide a ranking, on a host where a two-core grid cell is bimodal by launch. |
| `census-174e515a-vs-controls/` | 12:27:02–12:39:26 | candidate / `f2779605` / shipped 3.1.0, **every shape**, 2c-spin and 2c-park, 12 interleaved launches of 3 + 1 — the two-core half of the comparison, 576 launches, 0 unverified. |
| `bisect-f2779605-174e515a/` | 13:10:18–13:19 | the two one-core cells that moved the OTHER way, attributed: five builds along `f2779605..174e515a` and two scratch variants of the arena commit, 12 interleaved launches of 5 + 1 each — see the last section. |

### Shipped 3.1.0, the 09-13 release candidate and the candidate, same session (ns per unit, p50)

The field columns are `../savina-<shape>/` of this same session; `~` marks a candidate / `f2779605`
pair whose [min, p99] ranges overlap.

| shape (per unit) | config | shipped 3.1.0 | `f2779605` | **`174e515a`** | pass 2 | Δ vs 3.1.0 | Δ vs `f2779605` | min: 3.1.0 / `f2779605` / cand / pass 2 | CAF / SObjectizer / floor |
|---|---|---|---|---|---|---|---|---|---|
| `ping-pong` (round trip) | 1c-spin | 84.8 | 24.7 | **25.2** | 25.2 | -70.3 % / -70.3 % | +2.0 % / +2.0 % ~ | 83.9 / 24.3 / 24.4 / 24.8 | 264.4 / 138.1 / 3.8 |
|  | 1c-park | 85.6 | 24.9 | **25.2** | 25.5 | -70.6 % / -70.2 % | +1.1 % / +2.5 % ~ | 83.0 / 24.5 / 24.6 / 25.1 | 261.3 / 152.8 / 3.8 |
|  | 2c-spin | 301.5 | 198.3 | **195.1** | 193.1 | -35.3 % / -36.0 % | -1.6 % / -2.6 % ~ | 274.6 / 179.7 / 171.0 / 161.9 | 385.5 / 703.8 / 228.9 |
|  | 2c-park | 6,909.9 | 201.1 | **207.8** | 194.3 | -97.0 % / -97.2 % | +3.4 % / -3.4 % ~ | 6,781.6 / 186.0 / 191.4 / 163.7 | 386.1 / 5,604.7 / 4,416.4 |
| `counting` (message) | 1c-spin | 10.9 | 5.1 | **5.1** | 5.1 | -52.8 % / -53.0 % | +1.6 % / +1.3 % ~ | 10.7 / 4.9 / 4.8 / 5.1 | 75.4 / 64.5 / 4.2 |
|  | 1c-park | 10.8 | 5.1 | **5.0** | 5.2 | -53.4 % / -52.0 % | -0.9 % / +2.2 % ~ | 10.7 / 4.7 / 4.8 / 5.0 | 74.7 / 69.4 / 4.4 |
|  | 2c-spin | 21.1 | 6.8 | **7.1** | 6.9 | -66.2 % / -67.4 % | +4.8 % / +1.1 % ~ | 18.5 / 6.3 / 6.7 / 6.6 | 148.0 / 280.5 / 61.4 |
|  | 2c-park | 61.0 | 6.7 | **7.0** | 7.0 | -88.6 % / -88.6 % | +4.9 % / +4.9 % ~ | 48.8 / 6.5 / 6.5 / 6.6 | 152.6 / 172.4 / 27.3 |
| `thread-ring` (hop) | 1c-spin | 42.2 | 17.4 | **18.6** | 18.6 | -55.9 % / -55.9 % | +7.2 % / +7.1 % | 41.3 / 16.8 / 17.8 / 18.3 | 122.6 / 60.1 / 4.9 |
|  | 1c-park | 41.9 | 17.5 | **18.7** | 19.3 | -55.4 % / -54.1 % | +6.8 % / +10.1 % | 40.9 / 17.4 / 18.5 / 18.4 | 124.6 / 67.6 / 5.0 |
|  | 2c-spin | 150.5 | 97.8 | **89.4** | 92.8 | -40.6 % / -38.4 % | -8.5 % / -5.1 % ~ | 134.9 / 77.7 / 73.5 / 83.0 | 183.2 / 388.6 / 114.6 |
|  | 2c-park | 3,486.7 | 94.5 | **92.4** | 89.3 | -97.3 % / -97.4 % | -2.3 % / -5.5 % ~ | 3,419.1 / 81.2 / 81.0 / 74.6 | 184.4 / 184.7 / 2,447.2 |
| `fork-join` (message) | 1c-spin | 12.8 | 5.4 | **5.7** | 5.4 | -55.6 % / -58.0 % | +6.1 % / +0.3 % ~ | 12.6 / 5.0 / 5.3 / 5.3 | 156.8 / 46.7 / 27.4 |
|  | 1c-park | 12.6 | 5.7 | **5.7** | 5.4 | -54.9 % / -57.1 % | +0.4 % / -4.6 % ~ | 12.3 / 5.0 / 5.2 / 5.1 | 146.7 / 49.2 / 25.3 |
|  | 2c-spin | 13.5 | 5.7 | **5.9** | 5.8 | -56.1 % / -56.7 % | +3.1 % / +1.6 % ~ | 13.2 / 5.4 / 5.6 / 5.3 | 108.2 / 341.8 / 30.2 |
|  | 2c-park | 26.7 | 5.8 | **5.7** | 5.9 | -78.7 % / -78.1 % | -1.1 % / +1.6 % ~ | 14.8 / 5.1 / 5.5 / 5.7 | 106.9 / 209.7 / 23.0 |
| `big` (round trip) | 1c-spin | 23.9 | 12.9 | **12.7** | 12.7 | -46.8 % / -47.0 % | -1.3 % / -1.5 % ~ | 23.6 / 12.6 / 12.3 / 12.4 | 255.5 / 179.3 / 5.4 |
|  | 1c-park | 25.1 | 12.5 | **12.6** | 12.6 | -49.8 % / -49.7 % | +0.5 % / +0.8 % ~ | 24.9 / 12.3 / 12.5 / 12.4 | 255.3 / 181.4 / 5.5 |
|  | 2c-spin | 25.9 | 16.8 | **16.6** | 17.8 | -35.9 % / -31.2 % | -1.0 % / +6.3 % ~ | 23.2 / 15.0 / 14.5 / 15.2 | 374.6 / 502.6 / 48.3 |
|  | 2c-park | 28.3 | 17.3 | **16.2** | 15.7 | -43.0 % / -44.7 % | -6.3 % / -9.2 % ~ | 22.6 / 15.2 / 15.3 / 14.0 | 393.2 / 375.6 / 37.6 |
| `fib` (actor) | 1c-spin | 2,148.6 | 90.7 | **69.9** | 70.8 | -96.7 % / -96.7 % | -22.9 % / -22.0 % | 2,104.9 / 82.0 / 66.9 / 70.2 | 874.7 / 983.1 / 33.7 |
|  | 1c-park | 2,162.7 | 87.8 | **73.5** | 70.7 | -96.6 % / -96.7 % | -16.3 % / -19.4 % | 2,109.6 / 84.0 / 69.7 / 68.8 | 892.0 / 982.2 / 29.9 |
|  | 2c-spin | 2,002.6 | 56.8 | **45.0** | 44.0 | -97.8 % / -97.8 % | -20.7 % / -22.4 % | 1,928.4 / 54.7 / 42.9 / 42.2 | 955.9 / 2,424.2 / 32.0 |
|  | 2c-park | 1,982.8 | 56.2 | **50.4** | 45.0 | -97.5 % / -97.7 % | -10.3 % / -19.9 % | 1,862.0 / 52.9 / 42.7 / 42.6 | 989.9 / 2,481.8 / 29.6 |
| `chameneos` (meeting) | 1c-spin | 45.0 | 24.2 | **24.5** | 24.5 | -45.5 % / -45.5 % | +1.3 % / +1.3 % ~ | 42.7 / 22.5 / 23.9 / 24.3 | 531.3 / 299.7 / 14.7 |
|  | 1c-park | 44.4 | 24.8 | **24.2** | 24.6 | -45.5 % / -44.7 % | -2.5 % / -1.0 % ~ | 43.0 / 23.6 / 23.6 / 24.2 | 532.8 / 304.9 / 15.9 |
|  | 2c-spin | 75.1 | 61.8 | **40.2** | 41.1 | -46.4 % / -45.2 % | -34.9 % / -33.4 % ~ | 64.7 / 38.4 / 39.4 / 38.6 | 970.6 / 1,155.4 / 203.3 |
|  | 2c-park | 81.7 | 39.8 | **45.7** | 57.5 | -44.1 % / -29.7 % | +14.7 % / +44.4 % ~ | 68.7 / 37.8 / 39.0 / 39.8 | 934.4 / 798.7 / 119.5 |
| `bank-transaction` (transfer) | 1c-spin | 305.5 | 100.6 | **82.0** | 82.9 | -73.1 % / -72.9 % | -18.4 % / -17.6 % | 286.3 / 93.3 / 79.8 / 79.4 | 566.7 / 436.5 / 43.9 |
|  | 1c-park | 293.6 | 99.4 | **82.5** | 83.3 | -71.9 % / -71.6 % | -17.0 % / -16.3 % | 287.6 / 93.8 / 80.0 / 77.4 | 567.1 / 441.8 / 41.4 |
|  | 2c-spin | 262.1 | 83.2 | **77.2** | 66.0 | -70.6 % / -74.8 % | -7.3 % / -20.7 % ~ | 251.4 / 77.7 / 69.2 / 61.2 | 713.3 / 737.1 / 145.0 |
|  | 2c-park | 240.5 | 77.6 | **67.5** | 65.8 | -71.9 % / -72.6 % | -13.1 % / -15.2 % | 231.9 / 76.0 / 64.1 / 63.6 | 665.8 / 649.4 / 96.1 |

**Against 3.1.0 every one of the 32 cells is faster in both passes**, from −30 % (chameneos 2c-park,
81.7 → 45.7 / 57.5, a cell that is bimodal by launch here — the census below reads 80.7 → 40.9) to
−98 % (fib, whose 3.1.0 figure is a logging figure) and −97 % on the two park cells that cross a
core per message (ping-pong 6.91 µs → 0.2 µs, thread-ring 3.49 µs → 0.09 µs: the park handshake);
the geometric mean of candidate / shipped over the 32 cells is 0.24. **Against the field qb is the
fastest framework in all 32 cells** (geometric mean of qb / best rival 0.105; the narrowest cell is
ping-pong 2c-spin, 207 against CAF's 386 in the field run, 0.54), and it sits below the raw-thread
floor in 14 of the 16 two-core cells — the two above are fib (47 / 44 ns against 32 / 30: an actor
lifetime against a function call).

### The two-core cells, by census (median of the 12 launch medians [min … max], ns per unit)

| cell | qb | caf | baseline |
|---|---|---|---|
| ping-pong 2c-spin | 196.4 [181.2 … 213.2] | 386.6 [382.4 … 391.3] | 237.7 [210.3 … 250.2] |
| ping-pong 2c-park | 203.9 [177.8 … 219.0] | 388.0 [376.9 … 398.9] | — |
| thread-ring 2c-spin | 91.6 [79.7 … 96.7] | 191.9 [184.6 … 199.9] | 120.0 [99.1 … 140.0] |
| thread-ring 2c-park | 88.7 [78.0 … 99.7] | 189.9 [185.7 … 198.5] | — |

qb is under the raw-thread floor on both spin cells (196 against 238, 92 against 120) and at half
of CAF in all four.

| cell | cand | rc0913 | shipped |
|---|---|---|---|
| ping-pong 2c-spin | 201.0 [173.1 … 225.3] | 201.6 [177.6 … 219.2] | 295.3 [277.0 … 320.9] |
| ping-pong 2c-park | 204.1 [190.2 … 213.5] | 206.8 [188.2 … 225.2] | 6,913.7 [6,846.2 … 6,938.2] |
| counting 2c-spin | 6.8 [6.5 … 7.3] | 6.7 [6.5 … 7.8] | 20.4 [13.9 … 31.8] |
| counting 2c-park | 6.9 [6.6 … 7.5] | 6.7 [6.4 … 7.2] | 67.8 [60.3 … 76.8] |
| thread-ring 2c-spin | 88.2 [78.8 … 98.9] | 89.4 [79.5 … 100.6] | 151.6 [138.2 … 170.4] |
| thread-ring 2c-park | 87.8 [77.4 … 97.4] | 88.2 [75.1 … 99.5] | 3,489.8 [3,462.4 … 3,540.7] |
| fork-join 2c-spin | 5.9 [5.7 … 6.1] | 5.8 [5.5 … 6.0] | 14.1 [13.2 … 16.9] |
| fork-join 2c-park | 5.9 [5.6 … 6.1] | 5.8 [5.6 … 6.2] | 30.0 [17.3 … 40.8] |
| big 2c-spin | 16.0 [14.5 … 18.4] | 15.7 [15.1 … 18.7] | 25.5 [25.0 … 27.8] |
| big 2c-park | 16.2 [14.4 … 17.5] | 16.4 [14.4 … 18.3] | 29.2 [24.9 … 30.2] |
| fib 2c-spin | 46.7 [44.4 … 49.5] | 60.3 [56.5 … 63.7] | 2,052.8 [1,997.3 … 2,150.8] |
| fib 2c-park | 46.7 [43.9 … 48.6] | 58.5 [55.4 … 62.2] | 2,087.1 [1,966.5 … 2,113.8] |
| chameneos 2c-spin | 41.8 [39.7 … 74.1] | 40.8 [39.2 … 51.4] | 74.6 [67.2 … 103.7] |
| chameneos 2c-park | 40.9 [39.2 … 64.4] | 45.5 [38.2 … 68.8] | 80.7 [68.0 … 133.0] |
| bank-transaction 2c-spin | 65.1 [62.2 … 66.7] | 76.2 [73.2 … 89.9] | 245.5 [239.3 … 276.2] |
| bank-transaction 2c-park | 65.8 [64.5 … 69.2] | 76.3 [73.8 … 90.5] | 251.3 [234.7 … 270.8] |

The two shapes the late changes were made for move, with separated distributions — **fib 60.3 →
46.7 and 58.5 → 46.7 (−22 % / −20 %), bank-transaction 76.2 → 65.1 and 76.3 → 65.8 (−15 % / −14 %)** —
and every other two-core cell is level with `f2779605` (ping-pong 201.0 / 201.6, thread-ring 88.2 /
89.4, counting 6.8 / 6.7, fork-join 5.9 / 5.8, big 16.0 / 15.7; chameneos is bimodal by launch on
this host, 39 – 74, and its two builds overlap entirely).

### What the four late changes do on arm64 — and the two cells that went the other way

One core, where a grid cell is steady on this host (min – max 1 – 4 %): **fib 90.7 → 69.9 / 70.8
(−23 % / −22 %; park −16 % / −19 %)** is the arena, **bank-transaction 100.6 → 82.0 / 82.9 (−18 %;
park −17 % / −16 %)** the frame-free ask — the same signs and sizes the two x86-64 hosts measured
(TUNING §13.6, §13.7), on a platform whose allocator is fast (the arena still removes a `malloc` /
`free` pair per actor) and whose `thread_local` access is a call (`tlv_get_addr`). The ask-cost probe
(`qvoprobe-ask-cost`, 5 interleaved rounds of 2 s, candidate against `f2779605`): push 24.45 [24.26 –
24.58] against 24.97 [24.83 – 25.61]; **ask 37.44 [37.39 – 37.55] against 46.08 [45.90 – 47.15],
−18.8 %**; stream **21.16 [20.80 – 21.46] against 19.87 [19.44 – 20.48], +6.5 % per chunk**.

Two cells moved the other way, both small, both outside their spread, both attributed by a census
over five builds (`f2779605`, `a134ccd6` the arena, `7296ac8d` the pins, `6712ef30` the ask,
`174e515a`):

| cell | rc-f2779605 | arena-a134ccd6 | pins-7296ac8d | ask-6712ef30 | cand-174e515a |
|---|---|---|---|---|---|
| thread-ring 1c-spin | 16.9 [16.8 … 17.8] | 18.3 [17.9 … 18.6] | 18.3 [17.8 … 19.0] | 18.2 [18.0 … 18.8] | 18.3 [18.0 … 18.7] |
| thread-ring 1c-park | 17.2 [16.6 … 17.4] | 18.3 [17.9 … 18.9] | 18.4 [18.1 … 18.8] | 18.4 [18.0 … 18.9] | 18.4 [17.8 … 18.8] |
| ping-pong 1c-spin | 24.9 [24.6 … 25.4] | 25.1 [24.7 … 27.3] | 24.9 [24.5 … 25.5] | 25.2 [24.7 … 26.7] | 25.2 [24.5 … 26.5] |
| ping-pong 1c-park | 24.9 [24.5 … 25.3] | 24.9 [24.7 … 26.0] | 25.1 [24.8 … 26.3] | 25.5 [24.7 … 26.0] | 25.3 [24.8 … 25.6] |
| big 1c-spin | 12.7 [12.4 … 12.8] | 12.8 [12.6 … 12.9] | 12.7 [12.5 … 12.9] | 12.8 [12.5 … 13.1] | 12.7 [12.5 … 13.4] |
| big 1c-park | 12.6 [12.5 … 12.8] | 12.7 [12.6 … 12.9] | 12.7 [12.6 … 13.1] | 12.8 [12.6 … 13.3] | 12.7 [12.6 … 12.9] |

**thread-ring, one core: +1.2 – 1.4 ns per hop (+7 – 8 %), and the step is the arena's** — it appears
at `a134ccd6` and nothing after it moves the cell; ping-pong and big, the two other static shapes,
are flat across all five builds, so this is not a cost of the pass. Two scratch variants of
`a134ccd6` say what kind of cost it is (same census, 1c-spin):

| cell | rc-f2779605 | arena-a134ccd6 | a134ccd6-global-new | a134ccd6-granule64 |
|---|---|---|---|---|
| thread-ring 1c-spin | 17.2 [16.8 … 17.5] | 18.4 [18.1 … 18.7] | 17.2 [16.6 … 17.2] | 18.4 [18.0 … 18.7] |
| fib 1c-spin | 84.8 [82.9 … 100.3] | 70.1 [69.0 … 72.9] | 86.0 [82.4 … 91.2] | 70.6 [69.3 … 73.8] |

`a134ccd6-global-new` keeps the commit as it is — the class-level operators, the thread-local state
in every translation unit — and routes the four operators to the global allocator: **thread-ring
returns to 17.2, fib to 86.0**. So the step is where the hundred actor objects LIVE, not the code
around them; and it is not their alignment either — `a134ccd6-granule64` starts every actor on its
own cache line and reads the same 18.4 (and the same fib). What is left is locality with the rest of
the actor: under `malloc` an actor object sits beside what its constructor allocates (its
subscriptions, its scope), one after the other in the same zone; in the arena the object is in a
chunk of its own and its satellites are elsewhere, and a ring that touches a different actor every
18 ns pays for the second stream. Recorded with its instrument, not fixed: the 3.2 cell is 18.6
against 3.1.0's 42.2, the arena's gain is a quarter of an actor's lifetime, and the remedy (the
satellites in the arena too) is a design question — Huly carries it.

**The stream chunk, +1.3 ns (+6.5 %), is `qb::growable_ring`** (same five builds, `qvoprobe-ask-cost
stream`, 5 interleaved rounds: 19.50 / 19.52 / 19.83 / 20.22 / **21.13**, the step at the last
build). libc++'s `std::deque` packs 4096 bytes per block, like libstdc++'s 512 — it never had the
one-element-per-block tax QB-215 removed on MSVC (69 → 31 ns) — so on this host the ring has
nothing to win back and its wrap test costs a little. The trade stands as made (one container for
three standard libraries, no allocation per element on any of them); the figure is here so the next
reader does not rediscover it.

### The probes

- `qvoprobe-parked-timer-wake 1000 100 2000` — a 100 µs timer on a core parked at `setLatency(1 ms)`,
  the kqueue backend: lateness min 10.8 / p50 16.8 / p90 18.8 / p99 22.7 / max 35.5 µs. kqueue takes
  a `timespec`; the timer fires under the millisecond, as QB-196 asked (Linux reads ~60 µs).
- `qb-core-test-system-core-park-wake` ×10 (`ctest --repeat until-fail:10`, the root `release`
  build): 10 / 10.

### Residual load, since it is part of the measurement

Nothing of ours ran on the host during the window (no build, no suite, no other agent; the Linux
guest that measured `../../utm-debian13-arm64-g++14/` was idle, its own session started at 13:21
UTC). What did run: `WindowServer` ~46 % of a core throughout; Docker Desktop's
`Virtualization.framework` guest (other projects' containers) 27 % at the start, 3 % at the end;
Spotlight (`mds_stores`) 36 % at the end, indexing the session's own output. 1-minute load average
2.44 at the start and 2.87 at the end, on 14 logical cores.
