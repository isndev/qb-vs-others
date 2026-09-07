# qb branch `perf/pass-fixed-cost` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-182**, the second cut at residual 1 of `docs/TUNING.md` §13.3 (the cost of
a core pass that carries one event): the qb branch that walks the self pipe in place up to a
fence instead of swapping a second pipe in, resolves the outbound pipe of a `CoreId` with one
indexed load, scans the peer pipes inline before entering the flush drain, reads the io loop's
counters inline through qev's new `ev_active_count_addr()` / `ev_pending_count_addr()`, and
keeps the router's broadcast walk out of the unicast path — measured against the `develop` it
forks from (`0f7994e6`, the QB-180 head) on `savina/ping-pong`, `counting`, `thread-ring`,
`fork-join` and `big`, 4 configurations each, plus the four core `dev/bench` binaries and a
**new probe**, `tools/probes/pass-cost.cpp`. Same host, CPUs and build flags as the published
directories beside this one: `-O3 -DNDEBUG`, `taskset -c 0,2`, **9 repetitions + 2 warmup**,
qb-only builds, candidate / control / candidate in ONE quiet session on 2026-09-07, the Windows
side idle throughout. The candidate is `~/qvo/cand-pass`, built against the working tree that
became `2771cd67`; the control `~/qvo/ctl-0f7994e6`, a clean LF clone at `0f7994e6` (0 dirty).

| directory | qb at | what |
|---|---|---|
| `grid-2771cd67/`, `grid-2771cd67-pass2/` | **the branch head `2771cd67`** — measured first and third | **20 cells** each, qb only, all verified: five shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. 14:53:18–14:53:52 UTC. |
| `grid-0f7994e6/` | `develop` `0f7994e6` — the control, measured second | same 20 cells, same session. |
| `census/` | `2771cd67` vs `0f7994e6`, **12 interleaved launches** each, 3 reps + 1 warmup, on the four 2c cells of ping-pong and thread-ring and the three 1c anchors | 14:53:52–14:55:02 UTC. |
| `bench/` | the four core `dev/bench` binaries, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions, every iteration recorded) | 14:55:02–14:58:41 UTC. |
| `probe.txt` | `qvoprobe-pass-cost` k = 1, 2, 4, candidate and control alternated three times, CPU 2, 2 s windows | 14:58:41–14:59:17 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when the final candidate is measured on all eight shapes.

## The instrument: what a pass costs, and what each event adds

`tools/probes/pass-cost.cpp` hosts one actor on one pinned core and gives it k independent
self-event chains: every pass carries exactly k events through the self pipe, the router and the
handler, the handler counts them, and nothing reads a clock inside the window (the deadline is
checked every 65 536 events). With k = 1 and k = 2 the two unknowns separate — the per-event cost
is ns(2) − ns(1), the fixed per-pass cost 2·ns(1) − ns(2) — and k = 4 says whether they stay
linear. A `tick` mode counts passes through an `ICallback` for reference; it is NOT the fixed
cost alone, because the tick phase samples the wall clock for `LoopEvent::now` and an idle pass
reads the idle clock (the pacing QB-180 measured), and it reads 40 ns on the control for that
reason.

The control (`0f7994e6`), before any change: k = 1 **14.6 ns**, k = 2 **23.1**, k = 4 **41.0** —
a fixed pass of ~6 ns and **8.4–8.9 ns per event**, consistent with a one-core ping-pong round
trip of 28.3 ns being two such passes. The profile of that pass (cpu-clock, per instruction)
put a third of it in the handler's `push`: a four-deep dependent-load chain to find the outbound
pipe (the engine's core set → its index table → `_pipes`'s data pointer → the pipe's cursors), a
six-register prologue inherited from the inlined slow half of `allocate_back`, and the pipe's
cursors themselves just rewritten by `__receive__`'s swap; a fifth in `__receive__` (the swap:
six loads and six stores per pass, then `front()` reading what it had just written); and the
rest spread over `__flush_all__`'s prologue on a pass with nothing to flush, the two `ev_*`
calls of `has_work()`, the resolver's own prologue (the broadcast walk's snapshot vector,
inlined into the unicast path) and a division by 24 in the router's bounds check.

Each change was kept only after the probe moved, in this order (k = 1 / k = 2 / k = 4, ns per
pass, medians of three):

| step | k = 1 | k = 2 | k = 4 |
|---|---:|---:|---:|
| control `0f7994e6` | 14.55 | 23.15 | 40.99 |
| + inline io counters, swap gated on a non-empty self pipe | 14.46 | 23.35 | — |
| + pipe-of-core table, `allocate_back_slow` out of line | 14.08 | 20.88 | — |
| + the in-place walk up to a fence (no second pipe, no swap) | 13.80 | 20.24 | 33.25 |
| + inline peer scan before the flush drain, cold broadcast walk | 12.68 | 17.12 | 25.37 |
| + cached slot count in `key_table::find` (**the head**) | **12.9** | **16.6** | **24.6** |

So the pass with one event is **−12 %** and the marginal event **8.9 → 4.2 ns** (−53 %): the
batched shapes gain per message, the sparse ones per pass. The same probe on Windows / MSVC
19.51 reads 20.4 → 15.6 (k = 1), 28.4 → 22.9 (k = 2), 44.8 → 37.0 (k = 4).

## The grids, same session (p50 per unit, ns; candidate pass 1 / pass 2 against the control)

| cell | `0f7994e6` | **`2771cd67`** p1 / p2 | Δ |
|---|---:|---:|---:|
| ping-pong 1c-spin (round trip) | 28.68 | **22.59 / 22.80** | **−21 %** |
| ping-pong 1c-park | 28.44 | **22.93 / 22.59** | **−20 %** |
| ping-pong 2c-spin | 209.0 | 199.8 / 211.4 | −4 / +1 % |
| ping-pong 2c-park | 212.8 | 199.6 / 201.5 | **−6 / −5 %** |
| thread-ring 1c-spin (hop) | 21.14 | **17.15 / 17.45** | −19 / −17 % (the control's 21.1 is its upper mode; census 18.0 → 17.3) |
| thread-ring 1c-park | 17.93 | 17.19 / 17.27 | −4 % |
| thread-ring 2c-spin | 108.5 | 110.0 / 113.5 | **+1 / +5 %** — see below |
| thread-ring 2c-park | 108.4 | 112.3 / 112.7 | **+4 %** — see below |
| counting 1c-spin (message) | 8.89 | 8.03 / 7.67 | −10 / −14 % |
| counting 1c-park | 8.22 | 7.62 / 7.78 | −7 / −5 % |
| counting 2c-spin | 9.77 | 9.52 / 9.63 | −3 / −2 % |
| counting 2c-park | 9.80 | 9.83 / 9.30 | 0 / −5 % |
| fork-join 1c-spin (message) | 9.24 | 7.70 / 7.40 | **−17 / −20 %** |
| fork-join 1c-park | 9.04 | 7.87 / 7.51 | −13 / −17 % |
| fork-join 2c-spin | 9.49 | 9.14 / 8.24 | −4 / −13 % |
| fork-join 2c-park | 11.12 | 8.36 / 8.47 | **−25 / −24 %** |
| big 1c-spin (round trip) | 20.73 | 18.85 / 18.43 | −9 / −11 % |
| big 1c-park | 22.46 | 18.49 / 18.27 | **−18 / −19 %** |
| big 2c-spin | 20.57 | 18.13 / 18.71 | −12 / −9 % |
| big 2c-park | 20.62 | 18.45 / 18.47 | −10 % |

`census/`, twelve interleaved launches, medians (min … max): ping-pong 1c-spin **22.8** vs
28.4; thread-ring 1c-spin **17.3** vs 18.0; counting 1c-spin 7.6 vs 7.9; ping-pong 2c-spin
**203.9** (192.4 … 216.3) vs 211.5 (205.4 … 219.6); ping-pong 2c-park **197.8** vs 212.4;
thread-ring 2c-spin **112.8** (105.9 … 116.3) vs **107.6** (104.4 … 114.2); thread-ring 2c-park
112.3 vs 107.0.

### The one cell that moves the other way

thread-ring at two cores — 100 actors placed round-robin, so every hop crosses a core and one
token is ever in flight — reads **+1 to +5 %** against the control in three interleaved
censuses (a first ten-launch census +6 %, a later one +1 %, this one +5 %; the control itself
drifts 104–111 between sessions) on both hosts, while the two-actor ping-pong at two cores reads
−4 to −6 %. Bisected by rebuilding the head with one change undone at a time and running the
same census (`/tmp` variants, transcribed; eight launches each):

| thread-ring 2c-spin | ping-pong 2c-spin | |
|---|---:|---:|
| control `0f7994e6` | 107.1 | 212.2 |
| the head | 111.3 | 201.2 |
| head, flush drain entered every pass (no inline scan) | **105.1** | 200.6 |
| head, `__getPipe__` through `CoreSet::resolve` (no table) | 108.7 | 203.7 |
| head, broadcast walk inlined again | 113.0 | 200.2 |

It is the inline scan alone: a waiting core's idle pass is ~2 ns shorter without the drain's
prologue and walk, and the ring's cross-core hop — unlike ping-pong's — is slower for it, the
same sensitivity QB-180 measured when the idle spin pass lost its clock read (§14). Undoing the
scan costs the one-event pass 0.5 ns and the four-event pass 3 ns on the probe, and ping-pong
1c +6 %; it is kept, and the ring's ~4 ns per hop is recorded here as the price. Whether an idle
spin pass should be paced explicitly rather than by whatever work it happens to do is QB-181's
question; the ring is now its most sensitive instrument.

## `dev/bench` — the four core binaries (median of three run medians, ns; `bench/`)

| cell | `0f7994e6` | **`2771cd67`** | Δ |
|---|---:|---:|---:|
| `BM_PINGPONG<TinyEvent>` 64 actors, 1 core (per round trip) | 26.5 | **21.3** | **−20 %** |
| `BM_PINGPONG<TinyEvent>` 64 actors, 8 cores | 24.3 | 23.6 | −3 % |
| `BM_Mono_PingPong_Latency` (same-core round trip) | 63.7 | **58.5** | −8 % |
| `BM_Multi_PingPong_Latency` (cross-core) | 254.8 | 249.6 | −2 % |
| `BM_Reference_Multi_PingPong_Latency` (raw spsc, no qb code) | 188.0 | 196.4 | +4 % (186–197 both) |
| pipeline chain, 10 actors / 1 core (per delivery) | 32.2 | 29.9 | −7 % |
| pipeline chain, 8 actors / 8 cores | 314.0 | 309.3 | −2 % |
| `BM_Ask_RoundTrip_SameCore` | 42.8 | 41.7 | −3 % |
| `BM_Ask_RoundTrip_CrossCore` | 47.6 | 46.8 | −2 % |

Suites at the head, standalone `cmake -S qb`, 0 warnings: WSL2 g++-14 Release / ASan+UBSan /
TSan **192/192/0** each, run twice (before and after the last two steps). The Windows A/B is in
`../../desktop-b67osn6-win-msvc/qb-branch-perf-pass-fixed-cost/`.
