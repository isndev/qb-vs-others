# qb branch `perf/loop-clock-on-demand` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-180**: the residual the 3.2.0 candidate grid left first (`docs/TUNING.md`
§13.3, item 1 — the per-pass cost of a core with ONE event in flight), profiled and then fixed
on the qb branch this directory is named after, measured against the `develop` it forks from
(`43f62afe`, the candidate grid's build) on the three shapes that sit on that cost —
`savina/ping-pong`, `savina/counting`, `savina/thread-ring`, 4 configurations each — plus the
four core `dev/bench` binaries (`ping-pong-latency`, `pipeline-chain-latency`,
`ping-pong-throughput`, `ask-roundtrip`). Same host, CPUs and build flags as the published
directories beside this one: `-O3 -DNDEBUG`, `taskset -c 0,2`, **9 repetitions + 2 warmup**,
qb-only builds (`-DQVO_WITH_CAF=OFF -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF`; the
field is in `../savina-*/`), candidate / control / shipped 3.1.0 / candidate in ONE quiet session,
the Windows side idle throughout. The candidate is `~/qvo/cand-clock`, built against the working
tree; the control `~/qvo/ctl-43f62afe`, a clean LF clone at `43f62afe` (0 dirty); shipped is
`~/qvo/shipped` (v3.1.0). Two sessions on 2026-09-07, because the first one found a regression
the fix had to answer:

| directory | qb at | what |
|---|---|---|
| `grid-guard-only/`, `grid-guard-only-pass2/` | `43f62afe` + the tick-phase guard ALONE (the `LoopEvent` and its snapshot behind `if (!_callback_list.empty())`) — measured first and fourth | **12 cells** each, qb only, all verified: three shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. Session 1, 09:32:14–09:40:20 UTC. |
| `grid-43f62afe/` | `develop` `43f62afe` — the control of session 1, measured second | same 12 cells, same session. |
| `grid-shipped-3.1.0/` | v3.1.0, measured third | same 12 cells, same session. |
| `census-guard-only/` | guard-only vs `43f62afe`, **10 interleaved launches** each, 3 reps + 1 warmup, on the four 2c cells + the 1c ping-pong anchor | the instrument for the cross-core cells (§9.11); 09:41:26–09:42:24 UTC. |
| `census-variants/` | `43f62afe` / guard-only / five idle-pass variants, **7 interleaved launches** each — the third of three variant censuses, the one whose JSON survived (the first two were overwritten in `/tmp` and are transcribed below) | what settled the fix; 09:50:22–09:52:02 UTC. |
| `grid-c42abddf/`, `grid-c42abddf-pass2/` | **the branch head `c42abddf`**: the guard + the idle clock read on every idle pass in every latency mode — measured first and fourth | 12 cells each. Session 2, 09:55:27–10:03:31 UTC. |
| `grid-43f62afe-session2/`, `grid-shipped-3.1.0-session2/` | the control and shipped 3.1.0 of session 2, measured second and third | 12 cells each, same session. |
| `bench/` | the four core `dev/bench` binaries, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions, every iteration recorded) | right after session 2's grids, 10:03:31–10:07:14 UTC. |
| `census-final/` | `c42abddf` vs `43f62afe`, **15 interleaved launches** each, on the three 2c-spin cells, counting 2c-park and the 1c anchor | 10:07:36–10:08:24 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when the final candidate is measured on all eight shapes.

## What the profile said

`perf record -e cpu-clock -F 25000` (no PMU under WSL2) on `43f62afe`'s ping-pong at one core,
spin, 5 repetitions: **39.56 % `__vdso_clock_gettime`**, then `__workflow__` 9.51 %,
`__receive__` 9.24 %, `__receive_events__` 8.08 %, `EventResolver<Ball>::resolve` 6.93 %,
`ev_pending_count` 3.64 % + `ev_active_count` 2.15 % (`listener::has_work()`, every pass),
`send` 3.62 + 3.39 %, `__flush_all__` 2.97 %, `time()` 1.42 %. The cause is one line:
`VirtualCore::__workflow__` built `const qb::LoopEvent loop_ev{time(), _loop_count}` on every
pass — the tick-phase event — whether or not a callback was registered to receive it, so
`985cbb3a` ("`time()` samples the clock on demand") had moved the per-pass read from the top of
the loop into the tick phase rather than removed it. A core with no registered callback (every
benchmark here; every server that drives itself from io and events) paid ~13 ns of a ~33 ns pass
for an event nobody received.

## Session 1 — the guard alone: the one-core cells halve, the cross-core spin cells regress

p50 per unit, ns; guard-only pass 1 / pass 2 against the control, shipped 3.1.0 beside them:

| cell | shipped 3.1.0 | `43f62afe` | guard only p1 / p2 | Δ vs `43f62afe` |
|---|---:|---:|---:|---:|
| ping-pong 1c-spin (round trip) | 97.2 | 64.9 | **28.4 / 28.0** | **−56 %** |
| ping-pong 1c-park | 98.7 | 65.2 | **28.9 / 28.5** | **−56 %** |
| ping-pong 2c-spin | 266.5 | 206.9 | 253.2 / 259.2 | **+22 / +25 %** |
| ping-pong 2c-park | 26 850.6 | 215.1 | 210.4 / 212.7 | −2 / −1 % |
| thread-ring 1c-spin (hop) | 54.1 | 37.9 | **17.5 / 17.6** | **−54 %** |
| thread-ring 1c-park | 53.8 | 40.4 | **17.9 / 17.5** | **−56 %** |
| thread-ring 2c-spin | 140.9 | 105.9 | 133.3 / 134.3 | **+26 / +27 %** |
| thread-ring 2c-park | 13 612.6 | 111.6 | 110.5 / 111.3 | −1 / 0 % |
| counting 1c-spin (message) | 43.8 | 7.81 | 7.70 / 7.76 | −1 % |
| counting 2c-spin | 46.7 | 9.86 | 9.79 / 9.67 | −1 / −2 % |

`census-guard-only/`, ten interleaved launches: ping-pong 2c-spin guard-only **250.6 … 262.1**
(median 252.6) against `43f62afe` **198.2 … 212.9** (202.2); thread-ring 2c-spin **127.9 …
141.6** (133.6) against **101.4 … 111.4** (105.4) — fully separated distributions, not a level
shift between passes; the 2c-park pair overlaps (210.1 vs 208.4; 108.1 vs 113.0) and the 1c anchor
is 28.1 vs 64.7. A SPINNING core whose idle pass had just lost its only clock read polls its peer's
ring index in ~20 ns of unserialized code, and the cross-core exchange gets slower for it; a parked
core's idle pass still reads `mono_now()` for the idle-spin floor, and that cell did not move.

## The variant censuses — what an idle spin pass needs

Every variant is `43f62afe` + the guard + one insertion at the end of an idle spin-mode pass
(`!had_activity() && _mono_pipe_swap.empty()`), qb-only builds, interleaved launches, 3 reps + 1
warmup, CPUs 0,2. The per-primitive costs are from a pinned micro-benchmark on the same CPUs:
`_mm_pause` **33.4 ns** (i9-12900K P-core), `_mm_lfence` 2.8 ns, `clock_gettime` 13.1 ns
(`tsc` clocksource: `lfence; rdtsc` + the vvar page), bare `rdtsc` 6.0 ns.

Census 1 (7 launches, 09:43:36 UTC, transcribed):

| cell | `43f62afe` | guard only | + `spin_loop_pause()` |
|---|---:|---:|---:|
| ping-pong 2c-spin | 207.9 | 255.5 | 228.2 |
| thread-ring 2c-spin | 106.4 | 133.2 | 123.4 |
| ping-pong 2c-park | 209.9 | 213.0 | 213.3 |
| ping-pong 1c-spin | 65.0 | 28.2 | 29.1 |

Census 2 (5 launches, 09:46:48 UTC, transcribed):

| cell | `43f62afe` | guard only | + `pause` | + `lfence` | + tight `has_data()` poll ×256 | + poll with `pause` |
|---|---:|---:|---:|---:|---:|---:|
| ping-pong 2c-spin | 208.6 | 259.4 | 228.2 | 218.8 | **271.6** | 228.3 |
| thread-ring 2c-spin | 107.1 | 133.5 | 123.5 | 120.1 | **144.3** | 119.4 |
| ping-pong 1c-spin | 65.6 | 27.9 | 28.6 | 28.5 | 28.9 | 28.8 |

Census 3 (7 launches, `census-variants/`, 09:50:22 UTC):

| cell | `43f62afe` | guard only | + `lfence` | **+ `mono_now()` on the idle pass** | + `lfence` + `pause` | + `lfence` + `rdtsc` |
|---|---:|---:|---:|---:|---:|---:|
| ping-pong 2c-spin | 205.8 | 259.9 | 221.3 | **212.4** | 224.0 | 204.6 |
| thread-ring 2c-spin | 105.6 | 134.4 | 120.3 | **107.4** | 126.5 | 114.8 |
| ping-pong 2c-park | 214.0 | 211.7 | 213.1 | 214.2 | 213.2 | 213.7 |
| ping-pong 1c-spin | 65.0 | 28.3 | 28.6 | 28.6 | 28.4 | 28.6 |

Read together: the tightest poll is the worst (a bounded `has_data()` loop, +31 / +36 % over the
control), an `lfence` alone recovers most of the loss for 3 ns, a `pause` recovers less for 33 ns,
and ~10–15 ns of serialized work between two reads of the peer's index — the monotonic clock read
the park policy already takes on an idle pass, or a bare `lfence; rdtsc` — puts both cells back on
the control's figure. So the fix reads the idle clock on every idle pass in EVERY latency mode,
stamping `_idle_since` exactly as a parkable core does, and only the park stays gated on
`latency > 0`; a busy pass reads no clock in any mode. Documented as pacing in `VirtualCore.cpp`,
not as an accident; a hardware wait on the peer line (`umonitor`/`umwait`, `tpause`, arm64
`wfe`) is the axis this leaves open.

## Session 2 — the branch head against the control, same session (p50 per unit, ns)

| cell | shipped 3.1.0 | `43f62afe` | **`c42abddf`** p1 / p2 | Δ vs `43f62afe` | Δ vs shipped |
|---|---:|---:|---:|---:|---:|
| ping-pong 1c-spin (round trip) | 97.9 | 65.8 | **28.3 / 28.6** | **−57 %** | −71 % |
| ping-pong 1c-park | 98.1 | 66.5 | **28.4 / 28.4** | **−57 %** | −71 % |
| ping-pong 2c-spin | 272.8 | 197.1 | 211.1 / 212.9 | +7 / +8 % (see the census) | −23 % |
| ping-pong 2c-park | 26 976.2 | 219.4 | 209.3 / 211.9 | −5 / −3 % | −99 % |
| thread-ring 1c-spin (hop) | 54.7 | 38.1 | **17.8 / 17.8** | **−53 %** | −67 % |
| thread-ring 1c-park | 53.7 | 38.3 | 23.1 / **18.0** | −40 / **−53 %** | −57 % |
| thread-ring 2c-spin | 143.6 | 105.8 | 107.4 / 110.9 | +2 / +5 % (see the census) | −25 % |
| thread-ring 2c-park | 13 477.6 | 114.9 | 108.0 / 109.6 | −6 / −5 % | −99 % |
| counting 1c-spin (message) | 43.5 | 8.4 | 8.3 / 8.3 | −1 % | −81 % |
| counting 1c-park | 43.0 | 8.0 | 8.3 / 8.1 | +4 / +1 % | −81 % |
| counting 2c-spin | 46.1 | 9.5 | 10.4 / 9.9 | +9 / +4 % (see the census) | −77 % |
| counting 2c-park | 46.2 | 9.9 | 10.5 / 9.8 | +6 / −1 % | −77 % |

`census-final/`, fifteen interleaved launches, medians (min … max): ping-pong 2c-spin **209.7**
(200.9 … 216.5) against **205.6** (201.5 … 224.5); thread-ring 2c-spin **109.3** (101.9 … 113.7)
against **104.4** (100.9 … 108.5); counting 2c-spin 9.8 against 9.8, 2c-park 9.8 against 9.9;
ping-pong 1c-spin 28.2 against 64.8. The cross-core spin medians sit +2 / +5 % with overlapping
distributions — by `FAIRNESS.md` §1.5 no measurable difference, with a sign this document records
rather than hides: the branch's idle pass is a few nanoseconds shorter than the control's (it no
longer builds the empty tick snapshot), and the sweet spot above is not sharp enough to say more.

## `dev/bench` — the four core binaries (median of three run medians, ns; `bench/`)

| cell | `43f62afe` | **`c42abddf`** | Δ |
|---|---:|---:|---:|
| `BM_Mono_PingPong_Latency` (same-core round trip) | 100.2 | **62.9** | **−37 %** |
| `BM_Multi_PingPong_Latency` (cross-core) | 235.9 | 246.8 | +5 % over 3 runs; **257.3 vs 258.4** over eight interleaved runs (5 reps each, both 243–295 bimodal) |
| `BM_Reference_Multi_PingPong_Latency` (raw spsc, the ratio's denominator) | 190.1 | 192.5 | +1 % |
| pipeline chain, 10 actors / 1 core (per delivery) | 65.1 | **31.8** | **−51 %** (23.0 M → 47.1 M deliveries/s) |
| pipeline chain, 8 actors / 8 cores | 314.7 | 314.2 | 0 % |
| `BM_PINGPONG<TinyEvent>` 64 actors, 1 core (per round trip) | 27.9 | 26.4 | −5 % |
| `BM_PINGPONG<TinyEvent>` 64 actors, 8 cores | 25.2 | 23.7 | −6 % |
| `BM_Ask_RoundTrip_SameCore` | 44.6 | 42.8 | −4 % |
| `BM_Ask_RoundTrip_CrossCore` | 49.0 | 47.4 | −3 % |

`bench-compare.py` against `dev/bench/baseline/linux-wsl2-x86_64.json` on each candidate pass:
0 regressed, 9–10 improved, 17–18 within threshold of the 27 core-cell metrics (the run is VACUOUS
by design at that width — the recorded gate wants all 136; the full set is the release train's).

Suites at the head, standalone `cmake -S qb`, 0 warnings: WSL2 g++-14 Release / ASan+UBSan / TSan
**192/192/0** each, before and after the idle-clock half. The Windows A/B is in
`../../desktop-b67osn6-win-msvc/qb-branch-perf-loop-clock-on-demand/`.
