# qb `develop` `174e515a` — the 3.2.0 candidate on a native-arm64 Linux guest / Debian 13 / g++ 14.2

The Linux half of the 2026-09-19 session (the macOS half is
`../../macbook-m4pro-macos-clang21/qb-branch-develop/`), and this repository's first Linux host that
is not WSL2: a **UTM / QEMU guest with hardware virtualisation** (`systemd-detect-virt`: `qemu`) on
the Apple M4 Pro that is also the macOS host — Debian 13.7, kernel 6.12.107+deb13-arm64, 10 vCPUs,
11 GB, g++ 14.2.0, CMake 3.31.6, Ninja 1.12.1, `-O3 -DNDEBUG`, qb on its default `epoll` backend —
**pinned to vCPUs 2 and 4** (`--cpus 2,4`; Linux verifies the pin, and what it pins is a vCPU: the
host schedules the two vCPU threads where it likes, which is this guest's WSL2-shaped caveat and the
reason its two-core figures are read from a census too), **9 repetitions + 2 warmup**. Same
candidate, same two controls and same protocol as the macOS half: qb `develop` **`174e515a`**
against shipped v3.1.0 (`830ea244`) and against the release candidate of 2026-09-13 (`f2779605`),
in ONE quiet session on **2026-09-19, 13:21:14–14:25:59 UTC**, the macOS side idle (its own session
had ended at 13:19 UTC; no build, no suite and no benchmark ran there during the window — a few
single-threaded Python invocations of seconds did, while the `dev/bench` baseline was being
calibrated, during legs A and B). Three builds under `~/qvo` from `git archive` trees of the three
commits (13:19:13–13:20:08 UTC): `linux` — the full field, 33 binaries, 0 warnings in qb and the
adapters (3 in CAF 1.1.0's own sources under g++ 14, `-Wmaybe-uninitialized`); `shipped` (v3.1.0,
frameworks and probes off); `rc0913` (`f2779605`, probes on). CAF 1.1.0 and SObjectizer 5.8.5.1 are
the source trees the macOS build had fetched at those tags, copied over (`QVO_CAF_SOURCE_DIR`,
`QVO_SOBJECTIZER_SOURCE_DIR`): the guest's own `git clone` of CAF crawled at ~1 MB a minute.
`tools/negative-control.py --build ~/qvo/linux`: CAUGHT=7 CONFIRMED=4 MISSED=0. Every harness ran
with its repository checkout on ext4 as working directory (shipped 3.1.0's fib log).

| directory | when (UTC) | what |
|---|---|---|
| `grid-174e515a/` | 13:21:14–13:21:24 | **the candidate**, 32 cells, 32 / 32 verified — README.md's `framework=qb` grid for this host. |
| `grid-shipped-3.1.0-20260919/` | 13:21:24–13:30:08 | **the control**, v3.1.0 through the same adapters, 32 / 32 verified; nine minutes because its two park cells that cross a core per message cost the guest's futex wake — 29.6 µs a round trip, 14.8 µs a hop. |
| `grid-f2779605-20260919/` | 13:30:08–13:30:18 | the release candidate of 2026-09-13, the control for the four changes that landed after it, 32 / 32 verified. |
| `grid-174e515a-pass2/` | 13:30:18–13:30:28 | the candidate again, after the two controls — 32 / 32 verified. |
| `../savina-*/` (the field itself) | 13:30:28–13:45:59 | **132 cells, every framework** (qb at the candidate, CAF, CAF-detached, SObjectizer, the floor), 9 + 2, 0 unverified, 2 declared `n/a`; `../run.json`. |
| `census-174e515a-field/` | 13:45:59–13:47:22 | 12 interleaved launches of 3 + 1 on ping-pong and thread-ring, 2c-spin (qb / CAF / floor) and 2c-park (qb / CAF). |
| `census-174e515a-vs-controls/` | 13:47:22–14:24:58 | candidate / `f2779605` / shipped 3.1.0, every shape, 2c-spin and 2c-park, 12 interleaved launches of 3 + 1 — 576 launches, 0 unverified (37 minutes: shipped's two park cells again). |
| `bisect-f2779605-174e515a/` | 14:50:28–14:52:02 | the one cell this host reads differently, chameneos at two cores, over five builds and **24** interleaved launches; thread-ring and fib at one core over four builds, 12 launches of 5 + 1 — the last section. |

## Shipped 3.1.0, the 09-13 release candidate and the candidate, same session (ns per unit, p50)

The field columns are `../savina-<shape>/` of this same session; `~` marks a candidate / `f2779605`
pair whose [min, p99] ranges overlap.

| shape (per unit) | config | shipped 3.1.0 | `f2779605` | **`174e515a`** | pass 2 | Δ vs 3.1.0 | Δ vs `f2779605` | min: 3.1.0 / `f2779605` / cand / pass 2 | CAF / SObjectizer / floor |
|---|---|---|---|---|---|---|---|---|---|
| `ping-pong` (round trip) | 1c-spin | 129.2 | 21.8 | **22.0** | 21.9 | -83.0 % / -83.1 % | +0.8 % / +0.3 % ~ | 128.0 / 21.5 / 21.3 / 21.3 | 298.3 / 99.6 / 3.8 |
|  | 1c-park | 128.9 | 21.5 | **22.2** | 22.4 | -82.8 % / -82.6 % | +3.0 % / +4.1 % ~ | 128.0 / 20.4 / 21.7 / 21.7 | 296.4 / 132.1 / 3.7 |
|  | 2c-spin | 252.6 | 170.5 | **172.6** | 171.9 | -31.7 % / -32.0 % | +1.3 % / +0.8 % ~ | 244.8 / 158.8 / 156.3 / 147.0 | 291.4 / 741.8 / 215.0 |
|  | 2c-park | 29,563.0 | 178.5 | **163.1** | 180.6 | -99.4 % / -99.4 % | -8.6 % / +1.2 % ~ | 29,235.0 / 163.6 / 139.0 / 162.0 | 298.0 / 22,132.5 / 20,825.6 |
| `counting` (message) | 1c-spin | 14.6 | 5.9 | **6.0** | 5.9 | -59.2 % / -59.5 % | +0.1 % / -0.6 % ~ | 14.5 / 5.8 / 5.8 / 5.8 | 79.3 / 69.0 / 4.0 |
|  | 1c-park | 14.8 | 5.9 | **5.9** | 5.9 | -60.6 % / -60.4 % | -1.1 % / -0.7 % ~ | 14.4 / 5.8 / 5.8 / 5.8 | 78.3 / 72.0 / 3.9 |
|  | 2c-spin | 21.1 | 7.6 | **7.7** | 7.7 | -63.3 % / -63.5 % | +1.7 % / +1.2 % ~ | 16.6 / 7.2 / 7.2 / 7.3 | 195.9 / 123.6 / 57.6 |
|  | 2c-park | 36.9 | 7.4 | **7.3** | 7.4 | -80.1 % / -80.1 % | -1.0 % / -0.9 % ~ | 21.2 / 7.2 / 7.3 / 7.1 | 213.4 / 124.4 / 59.0 |
| `thread-ring` (hop) | 1c-spin | 51.8 | 24.9 | **24.2** | 24.3 | -53.3 % / -53.2 % | -3.0 % / -2.7 % | 51.0 / 24.7 / 23.9 / 24.0 | 143.5 / 47.7 / 4.8 |
|  | 1c-park | 52.3 | 25.0 | **24.7** | 24.2 | -52.8 % / -53.6 % | -1.4 % / -3.2 % ~ | 51.8 / 24.8 / 24.4 / 24.1 | 144.0 / 57.0 / 4.9 |
|  | 2c-spin | 111.0 | 82.9 | **79.8** | 80.6 | -28.1 % / -27.4 % | -3.8 % / -2.9 % ~ | 104.2 / 70.8 / 66.6 / 68.5 | 143.2 / 332.6 / 115.4 |
|  | 2c-park | 14,805.6 | 77.5 | **79.8** | 84.2 | -99.5 % / -99.4 % | +3.0 % / +8.6 % ~ | 14,707.6 / 64.8 / 62.7 / 72.1 | 150.5 / 181.4 / 10,647.5 |
| `fork-join` (message) | 1c-spin | 19.1 | 7.9 | **8.1** | 7.9 | -57.9 % / -58.9 % | +2.4 % / -0.2 % ~ | 18.7 / 6.7 / 7.3 / 6.8 | 235.0 / 47.8 / 4.1 |
|  | 1c-park | 19.1 | 7.8 | **7.3** | 6.8 | -61.8 % / -64.3 % | -6.2 % / -12.4 % ~ | 18.5 / 6.7 / 6.8 / 6.7 | 259.6 / 66.0 / 4.2 |
|  | 2c-spin | 19.9 | 8.1 | **8.1** | 8.3 | -59.4 % / -58.3 % | +0.2 % / +2.9 % ~ | 19.4 / 7.5 / 7.4 / 7.7 | 183.9 / 333.7 / 32.1 |
|  | 2c-park | 24.9 | 7.9 | **7.9** | 7.9 | -68.2 % / -68.5 % | -0.3 % / -1.1 % ~ | 24.0 / 7.7 / 7.7 / 7.8 | 297.9 / 272.5 / 19.5 |
| `big` (round trip) | 1c-spin | 22.2 | 16.3 | **15.9** | 15.8 | -28.4 % / -29.0 % | -2.1 % / -3.0 % ~ | 22.0 / 16.0 / 15.8 / 15.6 | 290.9 / 105.5 / 6.6 |
|  | 1c-park | 22.5 | 16.1 | **15.6** | 15.7 | -30.6 % / -30.0 % | -3.3 % / -2.4 % ~ | 22.2 / 15.8 / 15.5 / 15.6 | 291.6 / 115.3 / 6.7 |
|  | 2c-spin | 25.7 | 16.8 | **16.5** | 16.2 | -36.1 % / -37.2 % | -2.0 % / -3.8 % ~ | 24.0 / 14.4 / 13.9 / 15.3 | 253.5 / 269.2 / 44.1 |
|  | 2c-park | 23.8 | 16.3 | **15.9** | 16.5 | -33.1 % / -30.8 % | -2.6 % / +0.8 % ~ | 23.1 / 14.4 / 14.4 / 15.1 | 245.7 / 275.4 / 87.7 |
| `fib` (actor) | 1c-spin | 2,786.9 | 88.3 | **80.3** | 90.0 | -97.1 % / -96.8 % | -9.0 % / +1.9 % ~ | 2,637.1 / 87.5 / 77.4 / 85.1 | 1,000.5 / 1,715.5 / 30.1 |
|  | 1c-park | 2,813.2 | 86.1 | **80.1** | 83.0 | -97.2 % / -97.1 % | -7.0 % / -3.6 % ~ | 2,647.8 / 85.3 / 78.4 / 78.1 | 1,011.6 / 1,734.9 / 30.9 |
|  | 2c-spin | 2,217.0 | 62.0 | **51.8** | 51.2 | -97.7 % / -97.7 % | -16.5 % / -17.5 % ~ | 2,004.0 / 54.6 / 46.9 / 47.7 | 598.5 / 5,494.1 / 60.7 |
|  | 2c-park | 2,172.0 | 62.8 | **52.9** | 51.8 | -97.6 % / -97.6 % | -15.9 % / -17.6 % ~ | 2,006.3 / 53.6 / 46.4 / 47.6 | 576.0 / 5,794.3 / 31.5 |
| `chameneos` (meeting) | 1c-spin | 43.6 | 28.2 | **27.7** | 27.6 | -36.3 % / -36.6 % | -1.5 % / -2.0 % ~ | 42.2 / 28.0 / 27.3 / 27.1 | 636.3 / 184.5 / 13.1 |
|  | 1c-park | 43.3 | 39.3 | **27.7** | 28.2 | -35.9 % / -34.9 % | -29.4 % / -28.3 % | 42.2 / 38.9 / 27.5 / 27.8 | 629.6 / 214.4 / 14.1 |
|  | 2c-spin | 83.7 | 49.8 | **56.9** | 46.9 | -32.1 % / -44.0 % | +14.3 % / -5.9 % ~ | 71.0 / 42.4 / 43.4 / 42.2 | 630.8 / 741.7 / 211.2 |
|  | 2c-park | 84.0 | 55.0 | **66.9** | 71.4 | -20.3 % / -14.9 % | +21.6 % / +29.7 % ~ | 67.1 / 40.8 / 42.5 / 41.6 | 584.2 / 649.6 / 462.5 |
| `bank-transaction` (transfer) | 1c-spin | 238.9 | 94.3 | **78.6** | 79.8 | -67.1 % / -66.6 % | -16.7 % / -15.4 % | 224.7 / 91.6 / 76.7 / 78.6 | 670.4 / 346.5 / 34.9 |
|  | 1c-park | 232.8 | 94.2 | **79.2** | 80.7 | -66.0 % / -65.3 % | -15.9 % / -14.3 % | 225.3 / 91.0 / 77.9 / 78.5 | 692.8 / 382.0 / 36.2 |
|  | 2c-spin | 167.5 | 72.8 | **65.7** | 65.4 | -60.8 % / -61.0 % | -9.8 % / -10.2 % ~ | 156.0 / 70.5 / 61.8 / 62.9 | 687.7 / 489.4 / 143.0 |
|  | 2c-park | 169.8 | 73.6 | **64.5** | 63.7 | -62.0 % / -62.5 % | -12.4 % / -13.4 % ~ | 163.3 / 72.1 / 63.3 / 62.3 | 672.5 / 502.9 / 80.1 |

**Against 3.1.0 every one of the 32 cells is faster in both passes**: the geometric mean of
candidate / shipped is 0.23, the two park cells that cross a core per message go from the
hypervisor's futex wake to the spin floor's figure (ping-pong 29.56 µs → 0.16 – 0.18 µs,
thread-ring 14.81 µs → 0.08 µs — the WSL2 result, on another hypervisor and another architecture),
and the smallest gain is chameneos 2c-park (84.0 → 66.9 / 71.4, −15 % — the cell the last section is
about). **Against the field qb is the fastest framework in all 32 cells** (geometric mean of qb /
best rival 0.139; the narrowest is ping-pong 2c-spin, 167 against CAF's 291, 0.57) and below the
raw-thread floor in 15 of the 16 two-core cells (the one above is fib 2c-park, 50 against 32). The
park floor here is the guest's: `baseline__2c-park` reads 20.8 µs per ping-pong round trip and
10.6 µs per ring hop.

## The two-core cells, by census (median of the 12 launch medians [min … max], ns per unit)

| cell | qb | caf | baseline |
|---|---|---|---|
| ping-pong 2c-spin | 172.0 [161.7 … 180.5] | 297.4 [291.0 … 307.7] | 207.1 [183.9 … 224.3] |
| ping-pong 2c-park | 178.7 [161.1 … 189.6] | 297.3 [293.1 … 302.7] | — |
| thread-ring 2c-spin | 83.4 [71.5 … 95.0] | 143.7 [141.1 … 148.7] | 113.5 [94.0 … 130.2] |
| thread-ring 2c-park | 83.1 [77.5 … 87.3] | 144.5 [142.1 … 147.2] | — |

| cell | cand | rc0913 | shipped |
|---|---|---|---|
| ping-pong 2c-spin | 173.1 [160.3 … 189.8] | 170.5 [160.5 … 179.0] | 263.4 [247.1 … 282.6] |
| ping-pong 2c-park | 151.0 [142.2 … 167.9] | 153.9 [134.4 … 185.5] | 29,287.6 [28,008.1 … 29,508.6] |
| counting 2c-spin | 7.6 [7.2 … 8.4] | 7.7 [7.4 … 9.3] | 17.9 [16.1 … 21.3] |
| counting 2c-park | 7.5 [7.2 … 8.7] | 7.6 [7.3 … 9.2] | 34.1 [29.4 … 39.5] |
| thread-ring 2c-spin | 68.5 [65.2 … 87.7] | 70.5 [64.3 … 87.0] | 104.4 [96.9 … 129.7] |
| thread-ring 2c-park | 80.9 [65.9 … 91.1] | 82.0 [68.8 … 94.9] | 14,943.5 [14,707.1 … 15,181.5] |
| fork-join 2c-spin | 8.1 [7.7 … 8.7] | 8.1 [7.7 … 8.5] | 19.7 [19.3 … 20.4] |
| fork-join 2c-park | 8.0 [7.7 … 8.4] | 8.2 [7.8 … 9.0] | 23.4 [19.5 … 28.2] |
| big 2c-spin | 14.4 [13.6 … 16.4] | 14.6 [14.2 … 19.0] | 22.3 [21.7 … 24.4] |
| big 2c-park | 14.6 [14.0 … 19.0] | 14.8 [14.3 … 17.6] | 22.8 [22.2 … 24.5] |
| fib 2c-spin | 50.0 [47.6 … 55.4] | 62.5 [54.6 … 67.7] | 2,121.5 [1,855.4 … 2,274.8] |
| fib 2c-park | 50.4 [47.1 … 57.5] | 62.8 [53.9 … 68.4] | 2,144.9 [1,941.5 … 2,324.6] |
| chameneos 2c-spin | 57.6 [43.9 … 97.9] | 44.2 [42.8 … 76.3] | 79.7 [64.1 … 107.5] |
| chameneos 2c-park | 61.6 [42.8 … 93.3] | 49.4 [43.8 … 61.9] | 81.7 [67.7 … 104.7] |
| bank-transaction 2c-spin | 63.9 [60.9 … 71.1] | 73.0 [71.2 … 77.4] | 170.1 [167.0 … 180.4] |
| bank-transaction 2c-park | 64.7 [63.1 … 69.3] | 75.2 [72.1 … 81.1] | 170.9 [168.6 … 181.6] |

**fib 62.5 → 50.0 and 62.8 → 50.4 (−20 %), bank-transaction 73.0 → 63.9 and 75.2 → 64.7 (−12 % /
−14 %)**, the distributions separated or barely touching; ping-pong, counting, thread-ring,
fork-join and big are level with `f2779605`. Chameneos is not level — read on.

## What the four late changes do here

One core: **bank-transaction 94.3 → 78.6 / 79.8 (−17 % / −15 %)**, and the ask-cost probe
(`qvoprobe-ask-cost`, pinned to vCPU 2, 5 interleaved rounds of 2 s): push 21.50 against 21.81;
**ask 37.49 [37.45 – 37.56] against 51.69 [48.88 – 66.04], −27.5 %**; stream **19.79 against 19.82 —
level**: libstdc++'s deque packs 512 bytes a block, the ring neither wins nor loses here (it loses
1.3 ns on libc++, the macOS README; it won 2× on MSVC, TUNING §13.8). fib at one core is noisier on
this guest than anywhere else (80.3 and 90.0 in the two passes of one build), so it was given a
census of its own:

| cell | rc-f2779605 | arena-a134ccd6 | a134ccd6-global-new | cand-174e515a |
|---|---|---|---|---|
| thread-ring 1c-spin | 24.8 [21.0 … 25.0] | 24.1 [23.9 … 25.4] | 24.8 [24.6 … 25.0] | 24.1 [24.0 … 26.0] |
| fib 1c-spin | 89.0 [87.4 … 91.7] | 80.9 [79.1 … 89.8] | 91.5 [85.4 … 99.8] | 82.1 [79.0 … 91.5] |

**fib 89.0 → 80.9 / 82.1 (−9 % / −8 %)** with the arena, and 91.5 when the same commit's operators
are routed back to the global allocator; **thread-ring one core does NOT move the way it does on
macOS** — 24.8 → 24.1, the arena is if anything ahead — so that step is a property of the macOS
allocator's neighbourhood, not of the arena's layout.

## The cell this host reads differently: chameneos at two cores

The candidate's chameneos census above has the same fast mode as `f2779605` (43 – 45 ns a meeting)
and a heavier slow one: 6 of its 12 spin launches at 64 – 98 against 1 of 12, 7 of 12 park launches
at 61 – 93 against 1. A repetition of this shape is about nine milliseconds of work — 100 chameneos
and their mall created inside the window, then 200 000 meetings — short enough for a launch to be
decided by where things land when it starts, and twelve launches are few. Twenty-four, over five builds (`a134ccd6-global-new` is the arena
commit with its four class-level operators routed to the global allocator, everything else kept):

| cell | rc-f2779605 | arena-a134ccd6 | a134ccd6-global-new | ask-6712ef30 | cand-174e515a |
|---|---|---|---|---|---|
| chameneos 2c-spin | 50.3 [42.7 … 66.2] | 47.9 [42.1 … 99.0] | 49.5 [42.4 … 64.1] | 53.1 [42.0 … 82.5] | 48.8 [42.8 … 101.2] |
| chameneos 2c-park | 44.1 [42.9 … 66.2] | 55.4 [43.0 … 98.8] | 47.0 [42.8 … 82.3] | 55.2 [42.4 … 101.4] | 45.3 [40.7 … 92.9] |

Launches at or above 58 ns, of 24 — spin: `f2779605` 6, the arena 9, global-new 4, `6712ef30` 8, the
candidate 7; **park: `f2779605` 1, the arena 12, global-new 6, `6712ef30` 10, the candidate 9**. The
medians overlap everywhere and the fast mode is the same five times over; what the arena changes is
how OFTEN a launch falls into the slow mode, and how slow that mode is (up to ~100 ns against 66).
Attributed to the arena commit, and to the arena itself rather than to the code around it; NOT
explained — this guest has no `perf`, and the two pinned x86-64 hosts read the same shape inside its
spread when the arena was measured (TUNING §13.6), as does macOS (41.8 against 40.8 at the census). Recorded with its instrument beside the macOS thread-ring step: two small costs of the same
change, on the two hosts that were not there when it was measured. Huly carries both.

## The probes

- `qvoprobe-parked-timer-wake 1000 100 2000` — a 100 µs timer on a core parked at `setLatency(1 ms)`,
  epoll with `epoll_pwait2`: lateness min 2.5 / p50 55.4 / p90 59.0 / p99 67.5 / max 138.0 µs — the
  thread's 50 µs timer slack plus the wake, the figure QB-196 measured on WSL2 (kqueue on the macOS
  host reads 16.8).

## Residual load

The guest: an idle GitHub Actions runner service (no job ran — nothing was pushed during the
session), an `htop` at 0.5 % and the abandoned `git clone` of CAF at 0.4 % (killed after the
session; it moved no figure: the two candidate passes, 9 minutes apart on either side of the
controls, agree). 1-minute load average 2.18 at the start — the builds had ended 66 s before — and
1.14 at the end. The host: see the first paragraph.
