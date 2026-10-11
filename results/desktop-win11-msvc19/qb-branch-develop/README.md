# qb `develop` `43f62afe` — the 3.2.0 candidate grid, Windows 11 / MSVC 19.51

The Windows half of the **3.2.0 candidate grid** (the WSL2 half is
`../../wsl-debian-g++14/qb-branch-develop/`): qb `develop` at **`43f62afe`** — 29 commits over
the shipped v3.1.0 (`830ea244`), every perf branch of `docs/TUNING.md` §7–§12 merged (axes A–N,
QB-43 the segmented pipe, axis I the dense router, QB-174 the default-event registry, the
dense-table-growth chain fib produced, the five ask-path fixes bank-transaction produced and
QB-178 the ask slot table) — measured through the unmodified adapters on **all eight Savina
shapes**, 32 cells, against shipped 3.1.0 in ONE quiet session. Same host, CPUs and build flags
as the published directories beside this one: `/O2 /DNDEBUG`, CPUs 0 and 2 (two P-cores),
**9 repetitions + 2 warmup**, candidate / control / candidate on 2026-09-07,
**08:21:40–08:26:18 UTC**, no build, no test suite and no WSL measurement running anywhere on
the host (the WSL2 half ran afterwards, 08:45:23–08:54:36 UTC, with this side idle). The candidate is
`build/final` — the full field build, the same tree that measured CAF, SObjectizer and the floor
of the eight `savina-*/` directories — rebuilt at `43f62afe` (0 dirty; nine TUs recompiled, the
eight adapters and qb-core); the control is `build/shipped-win`, the v3.1.0 `git archive` build
every `savina-*/` directory's `qb` row comes from.

| directory | qb at | what |
|---|---|---|
| `grid-73018675/` | `develop` **`73018675`** — the 3.3.0 candidate, 92 commits over v3.2.1 (`build/wa-cnd`, a qb-only build of a `git archive` of that commit through the adapters of qb-vs-others `ffee4111`, MSVC 19.51.36256, 0 warnings), measured on 2026-10-11, **01:03:00–01:06:04 UTC** after 60 s of quiet (the WSL2 half ran first, 00:35–01:01 UTC, this side idle) | **40 cells** — the ten Savina wave-A shapes × four configurations — qb only, all verified: leg A of the wave-A session; `session.log` (the legs, `\Processor(_Total)` at both ends) and the three `run-leg-*.log` beside the documents. Their `framework_version` reads 3.2.1: `develop` reports the last tag until the train bumps it. The section at the end of this file reads it |
| `../savina-<shape>/` for the ten wave-A shapes | shipped **v3.2.1** `82ac0531` (`build/wa-shp`, the same harness tree), 01:06:04–01:19:37 | leg C: **the field**, 160 cells (qb, CAF 1.1.0, SObjectizer 5.8.5.1, the floor), 9 + 2, 0 unverified, merged into `../run.json` (`merged_partial_runs` 1) — the candidate's same-session control |
| `grid-73018675-pass2/` | `73018675`, 01:19:37–01:22:42 | leg A2: the candidate again, 40 / 40 verified |
| `../wave-a-form-sweep/` | 3.2.1 / CAF / SObjectizer, 01:22:42–01:27:56 | leg D: the forms the wave-A adapters declare pending a quiet-host measurement (FAIRNESS.md §1.1) — `concdict` and `concsll` at `form=1` for qb and CAF, `logmap`'s SObjectizer under `QVO_SO_GROUP_COOPS=1` at two cores — 18 side documents, all verified, never table cells |
| `census-73018675-vs-3.2.1/` | `73018675` against 3.2.1, 01:32:30–01:33:44 | leg E, a second quiet window (the WSL2 census ran 01:30:47–01:31:34, this side idle): 12 interleaved launches of 3 + 1 in AB/BA order on fork-join-create, `fib`, philosophers, concdict and logmap at four configurations and nqueens at two cores — 528 launches, 0 unverified, `census.log` per shape |
| `grid-f2779605/` | `develop` **`f2779605`** — **the RELEASE CANDIDATE** as it ships (`77b358d8` + the train's doc commits + QB-211's CMake; `src/` differs by comments only), measured on 2026-09-13, **03:20:04–03:20:18 UTC+2** (`build/final` rebuilt at it, 60 s of quiet; WSL2 idle, its own session ran afterwards) | **32 cells**, qb only, all verified — leg A of the 2026-09-13 session, README.md's Windows `framework=qb` grid since; every cell inside the launch spread of `grid-77b358d8/` (`docs/TUNING.md` §13.5) |
| `grid-shipped-3.1.0-20260913/` | v3.1.0 (`build/shipped-win`), 03:20:18–03:23:03 | **32 cells**, the same-session control of leg A — no candidate cell is slower; agrees with `grid-shipped-3.1.0-final/` within the launch spread |
| `../savina-*/` (the field itself) | the same `f2779605`, 03:23:03–03:30:22 | **132 cells, every framework** (qb, CAF, CAF-detached, SObjectizer, floor), 9 + 2, 0 unverified, 2 `n/a` — leg C: the published field re-measured in the candidate's session, `results/desktop-win11-msvc19/run.json` fresh, README.md's five Windows tables re-transcribed from it |
| `census-f2779605-field/` | the same, 03:30:22–03:32:16 | leg D: 12 interleaved launches of 3 + 1 on ping-pong and thread-ring, 2c-spin (qb / CAF / floor) and 2c-park (qb / CAF) — qb 186.8 on the floor's 180.8 for the ping-pong spin, under it (105.1 against 112.4) on the ring; §13.5's table |
| `grid-77b358d8/`, `grid-77b358d8-pass2/` | `develop` **`77b358d8`** — **the FINAL candidate**, 66 commits over 3.1.0 and 37 over `43f62afe`, measured first and third on 2026-09-09, **15:39:09–15:42:12 UTC** (the build of `build/final` at `77b358d8` ended 15:38:09, 0 dirty; the WSL2 half ran afterwards, 14:19:38–14:27:51 UTC, this side idle) | **32 cells** each, qb only, all verified — the release measurement; `docs/TUNING.md` §13.4 reads it against the midpoint grid below and against this session's control |
| `grid-shipped-3.1.0-final/` | v3.1.0 `830ea244` — the control of the final session, measured second | same 32 cells, same session; agrees with `grid-shipped-3.1.0/` within the launch spread |
| `census-77b358d8-vs-43f62afe/` | `77b358d8` (`build/final`) against `43f62afe` (`build/ab180-ctl`, the same adapters) | the interleaved launch census the bimodal two-core cells need: ten alternated launches of 3 + 1 per cell on CPUs 0 and 2, counting, chameneos, ping-pong and thread-ring 2c-spin, `census.log` the summary — level on counting (13.6 vs 13.5) and chameneos (66.9 vs 67.8, lower mode 46.8 vs 55.2), −22 % on ping-pong (189 vs 243), −10 % on the ring (107 vs 120). |
| `grid-43f62afe/`, `grid-43f62afe-pass2/` | `develop` **`43f62afe`** — **the candidate**, measured first (08:21:40–08:21:59 UTC) and third (08:25:59–08:26:18) | **32 cells** each, qb only, all verified: the eight shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. The first pass is the `framework=qb` grid README.md carries and `check-report.py` verifies. |
| `grid-shipped-3.1.0/` | v3.1.0 `830ea244` — **the control**, measured second (08:22:00–08:25:58 UTC; the two collapsed 2c-park cells and the logging fib are where its four minutes go) | same 32 cells, same session. Agrees with the published `savina-*/` `qb` rows within their spread (ping-pong 2c-park 7.79 µs here against 4.23 µs on 2026-09-04 — a collapsed cell has no stable figure). |

This is the grid that **joins the tables**: README.md's candidate grids for Windows and WSL2
render `grid-43f62afe/`, and `docs/TUNING.md` §13 reads the two hosts side by side. Every
earlier branch directory beside this one (`qb-branch-perf-*/`) is the A/B that produced one of
the 29 commits and stays as its provenance.

## Shipped 3.1.0 against the candidate, same session (ns per unit, p50)

The field columns are the published `../savina-<shape>/` cells (2026-09-04, -06 and -07, each
its own quiet session); the three qb columns are this session.

| shape (per unit) | config | shipped 3.1.0 | **`43f62afe`** | pass 2 | Δ (p50) | min: 3.1.0 / cand / pass 2 | CAF / SObjectizer / floor |
|---|---|---|---|---|---|---|---|
| `ping-pong` (round trip) | 1c-spin | 117.4 | **84.2** | 82.4 | -28.2 % / -29.8 % | 114.3 / 80.7 / 79.7 | 481.9 / 183.6 / 1.7 |
|  | 1c-park | 118.7 | **83.4** | 84.2 | -29.8 % / -29.1 % | 116.4 / 82.3 / 82.7 | 488.7 / 213.6 / 1.7 |
|  | 2c-spin | 373.9 | **297.2** | 295.5 | -20.5 % / -21.0 % | 334.7 / 257.9 / 255.6 | 485.4 / 922.5 / 182.8 |
|  | 2c-park | 7,793.0 | **274.5** | 299.8 | -96.5 % / -96.2 % | 6,899.4 / 240.4 / 260.9 | 490.0 / 1,034.6 / 468.9 |
| `counting` (message) | 1c-spin | 31.7 | **10.0** | 9.4 | -68.5 % / -70.3 % | 30.9 / 9.0 / 9.0 | 182.3 / 139.4 / 3.2 |
|  | 1c-park | 31.7 | **9.5** | 9.7 | -70.1 % / -69.3 % | 30.5 / 8.8 / 9.1 | 182.3 / 146.2 / 7.8 |
|  | 2c-spin | 34.3 | **11.7** | 11.9 | -65.7 % / -65.4 % | 33.6 / 11.3 / 11.5 | 126.8 / 289.4 / 42.3 |
|  | 2c-park | 34.2 | **11.7** | 11.9 | -65.8 % / -65.2 % | 33.8 / 11.3 / 11.2 | 150.0 / 307.6 / 53.1 |
| `thread-ring` (hop) | 1c-spin | 65.0 | **45.4** | 46.1 | -30.2 % / -29.2 % | 64.2 / 44.8 / 44.8 | 236.2 / 91.4 / 8.5 |
|  | 1c-park | 63.3 | **45.0** | 45.4 | -29.0 % / -28.4 % | 61.7 / 44.6 / 44.3 | 235.3 / 106.4 / 10.1 |
|  | 2c-spin | 209.2 | **154.2** | 172.9 | -26.3 % / -17.3 % | 194.4 / 136.7 / 162.9 | 238.6 / 481.9 / 109.9 |
|  | 2c-park | 2,621.0 | **146.4** | 149.7 | -94.4 % / -94.3 % | 2,160.2 / 141.2 / 137.9 | 236.3 / 473.9 / 270.8 |
| `fork-join` (message) | 1c-spin | 43.6 | **10.5** | 10.3 | -75.9 % / -76.3 % | 40.5 / 10.0 / 10.0 | 308.8 / 139.6 / 3.3 |
|  | 1c-park | 42.9 | **10.5** | 10.5 | -75.4 % / -75.4 % | 39.7 / 10.0 / 10.1 | 310.1 / 150.1 / 7.3 |
|  | 2c-spin | 46.1 | **12.2** | 11.2 | -73.5 % / -75.7 % | 43.1 / 10.9 / 10.7 | 171.0 / 289.1 / 38.2 |
|  | 2c-park | 44.5 | **11.2** | 11.3 | -74.9 % / -74.7 % | 42.8 / 10.7 / 10.8 | 168.1 / 268.4 / 48.5 |
| `big` (round trip) | 1c-spin | 37.1 | **20.3** | 20.2 | -45.4 % / -45.7 % | 35.9 / 19.4 / 19.9 | 496.6 / 186.6 / 11.2 |
|  | 1c-park | 36.0 | **20.0** | 20.2 | -44.3 % / -43.7 % | 35.2 / 19.1 / 19.3 | 501.8 / 199.5 / 19.4 |
|  | 2c-spin | 32.6 | **24.8** | 25.5 | -24.0 % / -21.8 % | 31.3 / 22.5 / 22.9 | 308.2 / 378.0 / 43.3 |
|  | 2c-park | 31.8 | **23.9** | 23.9 | -24.9 % / -24.6 % | 30.8 / 22.8 / 22.9 | 307.6 / 443.3 / 60.4 |
| `fib` (actor) | 1c-spin | 8,826.2 | **198.8** | 188.8 | -97.7 % / -97.9 % | 8,566.3 / 189.8 / 184.2 | 1,485.4 / 2,658.8 / 64.8 |
|  | 1c-park | 8,868.4 | **197.6** | 195.5 | -97.8 % / -97.8 % | 8,619.0 / 188.0 / 188.4 | 1,489.6 / 2,658.8 / 72.3 |
|  | 2c-spin | 8,175.1 | **117.2** | 118.9 | -98.6 % / -98.5 % | 8,023.3 / 116.4 / 113.7 | 918.6 / 3,066.9 / 70.4 |
|  | 2c-park | 8,251.8 | **116.5** | 118.2 | -98.6 % / -98.6 % | 7,944.1 / 111.9 / 112.7 | 910.7 / 3,079.8 / 70.8 |
| `chameneos` (meeting) | 1c-spin | 66.9 | **35.3** | 34.5 | -47.2 % / -48.5 % | 65.1 / 34.0 / 33.2 | 1,073.7 / 378.1 / 33.3 |
|  | 1c-park | 66.1 | **34.9** | 35.2 | -47.2 % / -46.8 % | 63.5 / 33.8 / 34.3 | 1,061.6 / 404.0 / 45.8 |
|  | 2c-spin | 100.3 | **65.3** | 63.5 | -34.9 % / -36.6 % | 96.5 / 58.9 / 61.2 | 1,082.8 / 1,299.5 / 315.8 |
|  | 2c-park | 105.2 | **63.5** | 61.7 | -39.6 % / -41.4 % | 100.7 / 59.6 / 58.1 | 1,068.8 / 1,450.0 / 372.0 |
| `bank-transaction` (transfer) | 1c-spin | 529.8 | **263.2** | 275.8 | -50.3 % / -47.9 % | 510.7 / 254.3 / 266.2 | 1,156.4 / 590.9 / 69.3 |
|  | 1c-park | 539.6 | **272.5** | 272.6 | -49.5 % / -49.5 % | 511.9 / 262.2 / 254.3 | 1,160.2 / 630.2 / 78.3 |
|  | 2c-spin | 749.6 | **156.6** | 157.0 | -79.1 % / -79.1 % | 342.5 / 147.8 / 145.8 | 1,150.4 / 759.9 / 643.2 |
|  | 2c-park | 701.4 | **149.6** | 157.4 | -78.7 % / -77.6 % | 374.9 / 144.9 / 149.1 | 1,161.7 / 887.6 / 144.9 |

## Reading

**All 32 cells move the same way, and none is inside the spread.** The smallest gain is
−17 % (thread-ring 2c-spin, pass 2, the one cell that is bimodal within a launch — below); the
one-core cells of the five static shapes are −28 to −76 %, the two collapsed 2c-park cells are
−96 % and −94 % (7.79 µs → 274 / 300 ns per round trip, 2.62 µs → 146 / 150 ns per hop — the
§5 defect, gone), fib is −98 % (505.8 → 11.4 ms at one core, 468.5 → 6.7 at two: 44 × / 70 ×,
the nine `LOG_INFO` lines per actor lifetime demoted and the O(n²) table growth fixed), and
bank-transaction is −50 % at one core and −79 % at two (37.5 / 35.1 ms → 7.8 / 7.5, over the
wide shipped 2c cells §12.4 records — 342–1133 ns per transfer across nine repetitions there,
145–208 here).

**Against the field, the candidate is the fastest framework in every one of the 32 cells**,
by 1.55 × (thread-ring 2c-spin against CAF's 238.6) to 16.8 × (chameneos 2c-park against
SObjectizer's 1 450), and it sits **below the raw-thread floor in eleven of the sixteen
two-core cells**: counting 11.7 vs 42.3 / 53.1, fork-join 12.2 / 11.2 vs 38.2 / 48.5, big 24.8 /
23.9 vs 43.3 / 60.4, chameneos 65.3 / 63.5 vs 315.8 / 372.0, ping-pong 2c-park 274.5 vs 468.9,
thread-ring 2c-park 146.4 vs 270.8, bank-transaction 2c-spin 156.6 vs 643.2 — the floor pays
one remote cache-line crossing per message on its SPSC ring where qb's staging pipe moves a
batch per flush (§9.7), and now that the batch is also cheap to build, the shapes with real
parallelism land under it. The five two-core cells that remain above the floor are the ones
that cross a core per message with nothing to batch: ping-pong 2c-spin 1.63 ×, thread-ring
2c-spin 1.40 ×, the two fib cells 1.65–1.67 × (creation, not messaging) and bank-transaction
2c-park 1.03 × (149.6 vs 144.9, level).

**What the grid leaves, named so the next axis is chosen from a figure and not a feeling:**

- **The one-core round trip is 84 ns, and a round trip is two dispatches.** counting measures
  the same dispatch at 10 ns when a burst of a million is staged and drained in batches;
  ping-pong's two actors alternate one event at a time, so every event pays a full
  `__flush_all__` → `consume_all` → route cycle of its own — 42 ns per hop against 10 batched.
  49 × the floor (1.7 ns, a `std::atomic` hand-off) and 2.2 × CAF. That cycle — the per-pass
  cost of a core with one event in flight — is the next thing to profile, on g++ where `perf`
  is.
- **thread-ring 2c-spin is bimodal within a launch, still.** Pass 1 sorts 136.7 … 178.0 ns per
  hop, pass 2 162.9 … 182.3; shipped 194.4 … 234.0. The grid's 154.2 / 172.9 is where the
  majority of nine fell, not a level — §9.11 already took this cell to an interleaved launch
  census (133.3 vs 138.8 on 2026-09-06), and that instrument, not the grid, is the figure to
  quote. ping-pong 2c-spin spreads the same way (255.6–352.7 in both passes) around 297 / 296.
- **The 1c bank-transaction cells sit 8–10 % above this morning's A/B session** (13.16 / 13.62
  ms here against 11.99–12.22 at 06:28 UTC in `../qb-branch-perf-ask-slot-table/`, two commits
  earlier on the same chain, a qb-only build). It is the host's level, not the build: the
  2c cells agree (7.83 / 7.48 against 7.11 / 7.18) and the launch census of 2026-09-06 saw the
  same 5–8 % shift move both sides of a pair alike. A session is compared within itself.
- **fib at one core is 199 ns per actor, 3.1 × the floor's 65** (the floor's node is one
  heap `Node` in a per-worker slot table and two ring messages, nothing else) — what an
  actor's lifetime costs once nothing is logged and the tables are dense: `addActor`, a
  registry slot, the two subscriptions, `kill`, and the pipe traffic of its two events.
  §11.3 lists what is left in it, and 7.5 × CAF's 1 485 is the margin it leaves.
- **MSVC's 2c cells are wide where g++'s are not** (§12.4): bank-transaction 2c-park carries
  one 233.8 / 222.0 outlier in nine on both passes, over a 145–165 body. Recorded, not
  explained.

At `43f62afe` all nine of qb's own GitHub lanes are green (`cmake`, `sanitize`,
`sanitize-thread`, `coverage`, `abi-fingerprint`, `install-consume`, `doc-lint`,
`format-check`, `scaffold`); the Windows/MSVC suite at the chain's last core commit `a61bded8`
is in `../qb-branch-perf-ask-slot-table/README.md` (Release 188 / 188 / 0, standalone SSL-off
qb). `docs/TUNING.md` §13 carries the two-host reading and the deltas against the WSL2 half.

## Wave A: shipped 3.2.1 against the 3.3.0 candidate on the ten new shapes (2026-10-11)

The Windows half of the wave-A session (the WSL2 half, and the same layout, is
`../../wsl-debian-g++14/qb-branch-develop/`): the candidate (leg A, `grid-73018675/`), the whole
field with **shipped qb v3.2.1** as its qb column (leg C, `../savina-<shape>/`), the candidate again
(leg A2, `grid-73018675-pass2/`), 9 + 2, CPUs 0 and 2, then the interleaved census in a second
quiet window (leg E, `census-73018675-vs-3.2.1/`). qb only, ns per unit, p50; "sep" marks a
candidate pass whose [min, p99] does not overlap the control's; the census column is the median
of the twelve launch medians, shipped → candidate. The field beside these cells is in
`../REPORT.md` and `docs/TUNING.md` §21.

| shape (per unit) | config | shipped 3.2.1 | **`73018675`** | pass 2 | Δ (p50), pass 1 / pass 2 | min: 3.2.1 / cand / pass 2 | census, shipped → candidate |
|---|---|---|---|---|---|---|---|
| `fork-join-create` (actor) | 1c-spin | 102.9 | **101.5** | 98.5 | -1.4 % / -4.3 % | 102.0 / 98.8 / 98.2 | 105.2 → 104.2 (-0.9 %, overlap) |
|  | 1c-park | 104.0 | **101.9** | 99.4 | -2.1 % / -4.5 % | 97.5 / 97.4 / 96.9 | 103.3 → 104.6 (+1.3 %, overlap) |
|  | 2c-spin | 54.1 | **57.3** | 58.1 | +6.0 % / +7.4 % | 52.3 / 55.7 / 53.9 | 56.6 → 58.0 (+2.6 %, overlap) |
|  | 2c-park | 55.8 | **55.6** | 57.9 | -0.4 % / +3.7 % | 54.4 / 53.2 / 56.2 | 56.1 → 57.0 (+1.8 %, overlap) |
| `nqueens` (solution) | 1c-spin | 13,398.6 | **13,526.6** | 13,774.6 | +1.0 % / +2.8 % sep | 13,225.6 / 13,422.4 / 13,565.0 | — |
|  | 1c-park | 13,363.7 | **13,723.6** | 13,643.1 | +2.7 % / +2.1 % | 13,303.8 / 13,472.9 / 13,550.7 | — |
|  | 2c-spin | 6,906.5 | **7,024.5** | 7,001.3 | +1.7 % / +1.4 % | 6,831.7 / 6,947.9 / 6,923.6 | 6,901.4 → 6,981.6 (+1.2 %, overlap) |
|  | 2c-park | 6,920.2 | **7,054.6** | 6,983.4 | +1.9 % / +0.9 % | 6,855.2 / 7,000.5 / 6,925.6 | 6,892.2 → 7,025.0 (+1.9 %, overlap) |
| `a-star` (node) | 1c-spin | 280.3 | **285.6** | 285.7 | +1.9 % / +1.9 % | 277.7 / 281.0 / 281.4 | — |
|  | 1c-park | 283.7 | **285.1** | 282.2 | +0.5 % / -0.5 % | 281.5 / 279.1 / 280.2 | — |
|  | 2c-spin | 151.8 | **152.9** | 154.1 | +0.7 % / +1.5 % | 150.6 / 150.6 / 151.2 | — |
|  | 2c-park | 153.6 | **161.6** | 154.0 | +5.2 % / +0.2 % | 151.8 / 156.8 / 151.3 | — |
| `philosophers` (meal) | 1c-spin | 69.0 | **67.7** | 64.1 | -1.9 % / -7.1 % sep | 65.8 / 65.4 / 63.4 | 66.0 → 65.2 (-1.2 %, overlap) |
|  | 1c-park | 67.3 | **66.3** | 64.7 | -1.4 % / -3.8 % | 65.5 / 64.9 / 63.4 | 66.2 → 65.6 (-0.9 %, overlap) |
|  | 2c-spin | 166.9 | **154.1** | 187.7 | -7.7 % / +12.5 % | 131.6 / 118.1 / 140.2 | 154.8 → 187.5 (+21.2 %, overlap) |
|  | 2c-park | 122.7 | **181.8** | 172.4 | +48.2 % / +40.5 % | 117.3 / 122.8 / 129.7 | 186.9 → 181.6 (-2.8 %, overlap) |
| `barber` (haircut) | 1c-spin | 2,872.2 | **2,912.5** | 3,010.6 | +1.4 % / +4.8 % | 2,845.2 / 2,845.9 / 2,901.7 | — |
|  | 1c-park | 2,861.0 | **2,889.3** | 2,883.3 | +1.0 % / +0.8 % | 2,841.2 / 2,846.9 / 2,867.2 | — |
|  | 2c-spin | 2,797.9 | **2,826.5** | 2,921.1 | +1.0 % / +4.4 % | 2,770.6 / 2,802.0 / 2,824.5 | — |
|  | 2c-park | 2,800.5 | **2,825.3** | 2,845.2 | +0.9 % / +1.6 % | 2,793.5 / 2,787.5 / 2,804.2 | — |
| `bndbuffer` (item) | 1c-spin | 13,110.0 | **12,982.5** | 13,064.1 | -1.0 % / -0.4 % | 12,996.2 / 12,958.5 / 13,020.7 | — |
|  | 1c-park | 13,146.2 | **12,984.1** | 13,022.1 | -1.2 % sep / -0.9 % | 13,104.4 / 12,955.4 / 13,000.9 | — |
|  | 2c-spin | 6,671.6 | **6,617.6** | 6,658.8 | -0.8 % / -0.2 % | 6,611.5 / 6,599.2 / 6,628.9 | — |
|  | 2c-park | 6,707.5 | **6,658.4** | 6,678.3 | -0.7 % / -0.4 % | 6,634.2 / 6,622.1 / 6,629.8 | — |
| `cigsmok` (round) | 1c-spin | 1,352.0 | **1,399.9** | 1,355.8 | +3.5 % / +0.3 % | 1,343.2 / 1,352.3 / 1,346.6 | — |
|  | 1c-park | 1,350.1 | **1,362.1** | 1,365.6 | +0.9 % / +1.1 % | 1,337.0 / 1,342.4 / 1,349.4 | — |
|  | 2c-spin | 1,174.1 | **1,168.5** | 1,173.0 | -0.5 % / -0.1 % | 1,159.7 / 1,155.8 / 1,161.4 | — |
|  | 2c-park | 1,173.2 | **1,199.4** | 1,189.6 | +2.2 % / +1.4 % | 1,153.9 / 1,154.6 / 1,173.6 | — |
| `concdict` (operation) | 1c-spin | 59.2 | **54.7** | 54.4 | -7.5 % / -8.1 % | 52.4 / 51.4 / 50.0 | 53.8 → 53.4 (-0.9 %, overlap) |
|  | 1c-park | 57.8 | **53.5** | 53.3 | -7.4 % / -7.8 % | 51.0 / 51.4 / 51.2 | 53.8 → 53.1 (-1.3 %, overlap) |
|  | 2c-spin | 63.3 | **69.3** | 68.4 | +9.5 % / +8.1 % | 56.3 / 59.2 / 57.4 | 64.6 → 70.3 (+8.9 %, overlap) |
|  | 2c-park | 72.1 | **66.0** | 66.8 | -8.4 % / -7.3 % | 64.3 / 64.0 / 64.1 | 64.0 → 68.6 (+7.1 %, overlap) |
| `concsll` (request) | 1c-spin | 21,120.4 | **21,046.3** | 20,898.7 | -0.4 % / -1.0 % | 20,927.7 / 20,775.2 / 20,796.5 | — |
|  | 1c-park | 21,014.7 | **20,939.6** | 21,073.6 | -0.4 % / +0.3 % | 20,909.5 / 20,822.4 / 20,950.7 | — |
|  | 2c-spin | 21,588.4 | **21,307.2** | 21,156.0 | -1.3 % / -2.0 % | 21,228.4 / 21,281.6 / 21,111.0 | — |
|  | 2c-park | 21,204.5 | **21,201.9** | 21,240.7 | -0.0 % / +0.2 % | 21,155.5 / 21,152.9 / 21,159.8 | — |
| `logmap` (term) | 1c-spin | 26.3 | **25.1** | 24.2 | -4.5 % / -7.9 % | 23.1 / 24.5 / 21.6 | 24.9 → 25.0 (+0.2 %, overlap) |
|  | 1c-park | 27.1 | **24.4** | 25.5 | -10.1 % / -6.1 % | 23.0 / 22.3 / 24.8 | 26.6 → 25.6 (-3.5 %, overlap) |
|  | 2c-spin | 19.8 | **20.5** | 19.9 | +3.6 % / +0.3 % | 17.7 / 18.9 / 19.4 | 21.1 → 21.0 (-0.8 %, overlap) |
|  | 2c-park | 21.1 | **19.1** | 19.7 | -9.6 % / -6.9 % | 20.1 / 17.1 / 19.4 | 18.6 → 18.8 (+1.1 %, overlap) |

**All forty cells level.** No census cell separates, and the grid's three single-pass "sep"
flags (nqueens 1c-spin pass 2 +2.8 %, philosophers 1c-spin pass 2 −7.1 %, bndbuffer 1c-park
pass 1 −1.2 %) are not repeated by the other pass.

- **fork-join-create at one core is level here** (−1.4 / −4.3 % in the grids, census −0.9 /
  +1.3 %, overlapping) — the cell the candidate loses by 19–25 % on WSL2 / g++ does not move
  on MSVC; `fib` is level at all four configurations (census −3.6 to +1.4 %). The fix that closes
  it on WSL2 (`../qb-branch-perf-fjcreate-regression/`, Huly QB-1009) measures level here too.
- **philosophers at two cores spans 117–213 ns per meal across the three grids** and 122–203
  per launch in the census, on both builds: the grid's +48 / +40 % at 2c-park and −8 / +12 % at
  2c-spin are which launch mode each pass fell in, and the census reads −2.8 % and +21.2 %,
  overlapping.
- **concdict**: in the grids −7 to −8 % at one core and at 2c-park, +8 to +10 % at 2c-spin; the
  census reads −0.9 / −1.3 % at one core and +8.9 / +7.1 % at two, all overlapping.
- **nqueens at two cores** carries the two levels of the WSL2 half (here ~6.9 and ~8.3 µs per
  solution); census +1.2 / +1.9 %, overlapping.
