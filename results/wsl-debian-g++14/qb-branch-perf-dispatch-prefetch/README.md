# qb branch `perf/dispatch-prefetch` — WSL2 Debian 13 / g++ 14.2 — measured, and NOT shipped

The investigation behind Huly **QB-198** (the dispatch loop prefetching the actor of an event ahead
while the current handler runs, on a core serving a population) — four sessions on 2026-09-08/09,
every one a quiet session (no build during the points, 60 s of quiet after the builds, the Windows
side idle, Docker Desktop quit), every tree from `git archive` into ext4 against this harness at
`d98b8dc`, same flags (`-O3 -DNDEBUG`), CPUs 0,2. The control everywhere is `develop` **`57df433d`**.
It ends in a negative result with numbers on both sides, which is why it is kept: the prefetch hid
the miss it targeted on the one workload that opened the issue and cost everything cheaper, and
no cheap gate tells the two apart. What shipped out of it is **QB-199** (one `listener::current`
reference per workflow loop, its own directory beside this one), the `dispatch-population` probe
and the `messaging-dispatch-batch` test.

| session | trees | what it decided |
|---|---|---|
| `census/` + `probe.txt` (21:58 UTC, 6 rounds) | `A` `5b839ab3` two-line actor prefetch, one event ahead, above 64 actors · `AB` `d0f69bef` + the router's handler slot through a virtual `IEventResolver::prefetch` · `ABC` `2f135c5d` + QB-199 | **B rejected**: `big` 1c +19 %, ping-pong 1c +3.7 %, `push` +13 % and `pass-cost` +21 % with the gate CLOSED — the code the call added around the loop. **A's gate at 64 too low**: `big`'s 120 actors are hot and paid +4 %. Under `perf`, A's trampoline fell 13.2 → 5.0 % of the core (the miss IS hidden) but `qb::ask` rose +2.7 points: two fills a peek take the fill buffers the handler's own misses need |
| `census-v2/` + `probe-v2.txt` (22:07 UTC, 8 rounds) | `C` `bd0858a2` QB-199 alone · `A1` `a685893a` C + ONE line, gate 512 · `A2` `254bb758` A1 two events ahead | every cell level or better for A2 (bank 1c −3.6 %, fib −2.4 %) — except counting **+1 %** for A1/A2, a per-event `_actor_count` compare never taken, on a 7.7 ns message |
| `census-a3/` + `probe-a3.txt` (22:18 UTC, 12 rounds) | `A3` `5e98dc9d` A2 with the loop as a C++20 templated lambda instantiated with and without the peek | **worse with the gate closed**: ping-pong 1c +12 %, counting +5 %, `big` +9.6 %, `push` +9.5 %, the probe at 16 actors +13 % — by-reference captures of the loop's locals and a doubled body; reverted |
| `census-a4/` + `probe-a4.txt` + `dispatch-population.txt` (22:25 UTC, 12 rounds) | `A4` `755a92db` the plain loop again, the gate a per-batch bool, a cursor keeping EIGHT events fetched ahead | the probe's verdict, below |

| file | what |
|---|---|
| `census/`, `census-v2/`, `census-a3/`, `census-a4/` | the eight Savina cells × trees × rounds (JSON), the build logs, each session's `census-summary.txt` |
| `probe*.txt` | `qvoprobe-ask-cost` `push` / `ask`, `qvoprobe-pass-cost` k = 1 — five alternations, 2 s, CPU 2 |
| `dispatch-population.txt`, `-summary.txt` | `qvoprobe-dispatch-population` (new here) — N actors of ~300 bytes on one core, batches of 256 events to actors drawn at random, ns per event, N = 16 … 16 384, ctl / A4 × 3, 1.5 s, CPU 2 |

None of it is merged into the published tables.

## Why (the profile that opened the issue)

`perf` on the control, savina/bank-transaction 1c, 100 repetitions, 13 K samples: the account's
`on(Deposit&)` handler was **18 %** of the core and **78.6 % of its samples sat on the `jne` after
`cmpb $0,0x14(%rdi)`** — the trampoline's `is_alive()` load, the destination actor's first line, an
L3 miss (1 000 accounts, ~1 MB of working set against a 1.25 MB L2, each account touched every
~1 000 events) — with 14.5 % more on the same line's next load; `EventResolver<Deposit>::resolve`
(5 %) had 64 % of its samples on the load of the `semh` slot. The transfer coroutine (25 %) sat on
the adapter's `std::deque` chunk — the user's structure, left alone. `__tls_init` was 2.2 % of
ping-pong 1c: `listener::current` reached through g++'s TLS wrapper three times a pass.

## Session 1 (`census/`, medians of six interleaved rounds, ns per unit)

| cell | `ctl` | `A` | `AB` | `ABC` |
|---|---:|---:|---:|---:|
| bank-transaction 1c | 144.3 | **140.0** (−3.0 %) | 142.4 | 142.6 |
| bank-transaction 2c | 82.4 | 84.7 | 86.2 | 86.7 (spreads 79–91, overlapping) |
| big 1c (120 actors) | 18.4 | **19.1 (+4.1 %)** | **21.9 (+19 %)** | 22.0 |
| counting 1c | 7.6 | 7.7 | 7.7 | 7.7 |
| fib 1c (57 k actors) | 130.8 | 128.4 (−1.8 %) | 129.9 | 128.9 |
| ping-pong 1c | 23.1 | 23.3 | **24.0 (+3.7 %)** | 23.1 |
| ping-pong 2c | 157.8 | 154.3 | 159.5 | 154.0 |
| thread-ring 2c | 76.9 | 75.1 | 76.3 | 75.7 |

Probes (medians of five): `ask` 47.16 / 46.50 / 46.36 / 46.26; `push` 24.47 / 24.44 / **27.76** /
27.28; `pass-cost` k = 1 12.46 / 12.58 / **15.03** / 14.75 — B's cost on a pass that never enters
its gated block, the reason it is gone.

## Session 2 (`census-v2/`, medians of eight interleaved rounds)

| cell | `ctl` | `C` | `A1` | `A2` |
|---|---:|---:|---:|---:|
| bank-transaction 1c | 147.3 | 146.1 | 143.0 (−2.9 %) | **142.0 (−3.6 %)** |
| bank-transaction 2c | 83.8 | 85.5 | 83.1 | 82.2 (−1.9 %) |
| big 1c | 18.1 | 18.2 | 18.0 | 18.2 (level, 17.9–18.5 both) |
| counting 1c | 7.6 | 7.6 | 7.7 (+0.8 %) | 7.7 (+1.2 %) |
| fib 1c | 129.2 | 131.8 | 128.2 | 126.1 (−2.4 %) |
| ping-pong 1c | 23.0 | **22.7 (−1.6 %)** | 23.2 | 22.8 |
| ping-pong 2c | 153.9 | 155.1 | 153.4 | 154.3 |
| thread-ring 2c | 75.2 | 75.3 | 74.0 | 74.2 (−1.3 %) |

Probes: `ask` 46.93 / 46.25 / 46.97 / 46.19; `push` 24.30 / 24.01 / 24.05 / 23.64;
`pass-cost` k = 1 12.41 / 12.36 / 12.32 / 12.17.

## Session 3 (`census-a3/`, twelve rounds): the templated lambda

| cell | `ctl` | `A3` |
|---|---:|---:|
| bank-transaction 1c | 146.0 | 146.1 |
| big 1c | 18.1 | **19.9 (+9.6 %)** |
| counting 1c | 7.4 | **7.8 (+5.0 %)** |
| ping-pong 1c | 22.9 | **25.7 (+12.2 %)** |
| fib 1c | 127.3 | 126.6 |

Probes: `ask` 46.14 → 48.27 (+4.6 %), `push` 23.87 → **26.13 (+9.5 %)**, `pass-cost` 12.31 → 13.15
(+6.8 %). The loop wrapped in `[&]<bool Peek>()` so that a peek-free instance could be compiled
"byte for byte" was an assumption, not a measurement; the compiler disagreed.

## Session 4 (`census-a4/` + the probe, twelve rounds): the plain loop, eight events ahead

| cell | `ctl` | `A4` | quartiles ctl / A4 |
|---|---:|---:|---|
| bank-transaction 1c | 146.3 | 145.5 (−0.5 %) | 145.0–148.6 / 143.6–146.4 |
| bank-transaction 2c | 82.1 | 82.5 | 81.0–83.9 / 79.4–85.4 |
| big 1c | 18.1 | **18.5 (+1.9 %)** | 17.9–18.4 / 18.3–18.6 |
| counting 1c | 7.4 | 7.5 (+0.8 %) | 7.4–7.5 / 7.4–7.5 |
| fib 1c | 128.2 | 125.8 (−1.9 %) | 126.8–130.6 / 124.7–128.6 |
| ping-pong 1c | 23.0 | 22.7 (−1.4 %) | 22.8–23.1 / 22.6–22.7 |
| ping-pong 2c | 154.0 | 151.0 (−2.0 %) | 151.6–157.2 / 148.4–153.0 |
| thread-ring 2c | 74.1 | 73.2 (−1.1 %) | 73.5–74.4 / 71.6–74.4 |

Probes: `ask` 46.10 / 46.21, `push` 23.87 / 23.26, `pass-cost` 12.28 / 12.34.

**`dispatch-population`** (ns per event, medians of three, ctl / A4 — the gate opens above 512):

| actors | `ctl` | `A4` | Δ | ranges |
|---:|---:|---:|---:|---|
| 16 | 5.04 | 5.35 | **+6.2 %** | 5.02–5.07 / 5.30–5.49 |
| 64 | 5.12 | 5.44 | +6.3 % | 5.10–5.43 / 5.42–5.51 |
| 256 | 5.49 | 5.80 | +5.6 % | 5.48–5.53 / 5.69–5.90 |
| 512 | 5.68 | 7.13 | **+25.5 %** | 5.67–5.84 / 7.11–7.14 |
| 1 024 | 6.06 | 7.36 | **+21.5 %** | 5.92–6.07 / 7.33–7.53 |
| 4 096 | 9.32 | 8.99 | −3.5 % | 9.29–9.34 / 8.98–9.02 |
| 16 384 | 11.22 | 12.08 | +7.7 % | 11.16–11.58 / 12.03–12.11 |

## Reading

The control's own curve is the fact that decides it: from 5.0 ns an event at 16 actors to 11.2 at
16 384, the population costs a core ~6 ns an event on this probe — and an L3 miss on this host is
40–60 ns. The out-of-order engine already overlaps the misses of a dozen short, predictable
handlers (one trampoline target, one loop, a 512-entry reorder window), so on every cheap workload
the software prefetch adds its instructions and hides nothing: **+6 % an event with the gate
closed** (the per-batch bool, the cursor's bookkeeping), **+21–25 % on a hot population of 512–1 024
actors** (300 KB of actors sit in L2; the peek's own loads and fills cost more than the L2 hits they
pre-empt), −3.5 % where the misses finally start (4 096), +7.7 % again at 16 384 (the fill buffers).
Where the prefetch paid — savina/bank-transaction, a ~100 ns handler with dependent misses and
unpredictable control flow (a coroutine resumed inline, indirect calls to different targets) — it
paid at TWO events of lead (−3.6 %) and not at eight (−0.5 %: 800 ns of bank's own traffic evicts
the fetched lines before use), so no single lead serves both, and no cheap gate — population,
batch size — tells a bank-like handler from a hot population of the same size. The one gate that
would (the time an event costs, read per batch) needs `rdtsc` per batch, which a ping-pong's batch
of one cannot afford, and a threshold tuned on this host. Not shipped. The instrument stays: the
probe's control curve is the cost of a population on this host, and the batch test pins what the
loop does with every destination shape.

**QB-199** (`C`), measured alone at twelve rounds in `../qb-branch-perf-loop-listener-ref/`.
