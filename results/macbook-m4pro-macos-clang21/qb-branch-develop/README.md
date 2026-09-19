# qb `develop` `174e515a` — the 3.2.0 candidate on macOS / Apple M4 Pro / AppleClang 21

The macOS third of the **3.2.0 candidate grid** (the two others are
`../../desktop-b67osn6-win-msvc/qb-branch-develop/` and `../../wsl-debian-g++14/qb-branch-develop/`),
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

## Shipped 3.1.0, the 09-13 release candidate and the candidate, same session (ns per unit, p50)

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

## The two-core cells, by census (median of the 12 launch medians [min … max], ns per unit)

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

## What the four late changes do on arm64 — and the two cells that went the other way

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

## The probes

- `qvoprobe-parked-timer-wake 1000 100 2000` — a 100 µs timer on a core parked at `setLatency(1 ms)`,
  the kqueue backend: lateness min 10.8 / p50 16.8 / p90 18.8 / p99 22.7 / max 35.5 µs. kqueue takes
  a `timespec`; the timer fires under the millisecond, as QB-196 asked (Linux reads ~60 µs).
- `qb-core-test-system-core-park-wake` ×10 (`ctest --repeat until-fail:10`, the root `release`
  build): 10 / 10.

## Residual load, since it is part of the measurement

Nothing of ours ran on the host during the window (no build, no suite, no other agent; the Linux
guest that measured `../../utm-debian13-arm64-g++14/` was idle, its own session started at 13:21
UTC). What did run: `WindowServer` ~46 % of a core throughout; Docker Desktop's
`Virtualization.framework` guest (other projects' containers) 27 % at the start, 3 % at the end;
Spotlight (`mds_stores`) 36 % at the end, indexing the session's own output. 1-minute load average
2.44 at the start and 2.87 at the end, on 14 logical cores.
