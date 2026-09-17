# qb branch `perf/actor-arena` — WSL2 Debian 13 / g++ 14.2

The WSL2 half of the A/B for Huly **QB-212, point 1** (the Windows half is
`../../desktop-b67osn6-win-msvc/qb-branch-perf-actor-arena/`): the qb branch on which the actor
object comes from a per-thread size-class arena (`qb::Actor::operator new` / `operator delete`
over `qb::allocator::thread_arena`: 16-byte classes up to 1 KiB, LIFO reuse per class, chunks
from `slab_cache`, no lock and no TLS init guard on the path) instead of `malloc`. The
measurement it answers is TUNING §13.5: after the 3.2 train the actor object's own `new` /
`delete` was the last heap traffic of an actor lifetime — one `malloc` per actor, 27.8 % of the
core on `fib` here — and `fib` was the one-core cell where qb lost most against the raw-thread
floor.

Same host, CPUs and flags as the published directories beside this one: `-O3 -DNDEBUG`, native
arch off, CPUs 0,2, **9 repetitions + 2 warmup** per grid cell. Control = `~/qvo/linux` (qb
`develop` `f2779605`, the 2026-09-13 build of the 3.2.0 candidate, its `fib` binary dated
2026-09-13 01:32 UTC, not rebuilt); candidate = `~/qvo/arena`, qb only, against a `git archive`
of the branch's final commit `385bdb37` (`bff6baf2` = the arena, `37d95467` = the orphan list of
the exit path and ASCII comments, `385bdb37` = a g++-only fix in the state struct's initialisers —
the hot path is byte-identical across the three). ONE quiet session on 2026-09-17, Windows idle,
Docker Desktop stopped: candidate / control / candidate grids, then the interleaved launch
censuses, 02:36:20–02:36:57 UTC, right after the `dev/bench` gate of the same session.

| directory | qb at | what |
|---|---|---|
| `grid-cand-385bdb37-pass1/`, `grid-cand-385bdb37-pass2/` | **the branch** — measured first and third | **32 cells** each, qb only, all verified: eight shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. |
| `grid-ctl-f2779605/` | `develop` `f2779605` — the control, measured second | same 32 cells, same session. |
| `census/` | branch vs `f2779605`, **12 interleaved launches** (fib, four configs) and **8** (the 1c-spin anchors bank-transaction, ping-pong, counting), 3 reps + 1 warmup each | |
| `bench/` | the `dev/bench` record of the session: the gate against the 2026-09-07 baseline, the in-session control comparison of the flagged binaries, and the layout proof (below) | |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when it is merged and the final candidate is measured on all eight shapes.

## fib — the cell the branch is for (census, median of the per-launch medians, ns per actor lifetime)

| cell | `f2779605` | **branch** (`385bdb37`) | Δ |
|---|---:|---:|---:|
| fib 1c-spin | 129.2 (126–134) | **99.1** (96–100) | **−23 %**, distributions separate |
| fib 1c-park | 128.8 (126–131) | **100.7** (98–102) | **−22 %**, separate |
| fib 2c-spin | 86.3 (84–89) | **57.9** (56–63) | **−33 %**, separate |
| fib 2c-park | 89.2 (85–96) | **58.3** (56–62) | **−35 %**, separate |

The anchors, same session, 8 interleaved launches: bank-transaction 1c-spin 145.6 vs 141.5
(−3 %, overlap — the ask path allocates nothing per ask, so the arena cannot touch it: that is
QB-212 point 2), ping-pong 1c-spin 22.9 vs 23.0 (+1 %, overlap), counting 1c-spin 7.6 vs 7.7
(overlap).

Why the one-core gain is smaller here than on Windows (−23 % against −31 %): glibc's `malloc`
fast path is cheaper than the Windows heap's (§13.5 measured the control lifetime at 124 ns here
against 179 there), so the same removed pair is a smaller share of a shorter lifetime; the
two-core cells, where the lifetime is split across two threads' heaps, gain the same −33 to −35 %
on both hosts.

## The grids (p50 per unit, ns; candidate passes against the control)

| cell | `f2779605` | **branch** p1 / p2 | Δ p1 / p2 |
|---|---:|---:|---:|
| fib 1c-spin (actor lifetime) | 123.98 | **99.48 / 100.95** | **−20 / −19 %** |
| fib 1c-park | 125.05 | **101.51 / 98.95** | **−19 / −21 %** |
| fib 2c-spin | 84.32 | **56.66 / 59.28** | **−33 / −30 %** |
| fib 2c-park | 87.42 | **57.09 / 57.17** | **−35 / −35 %** |
| ping-pong 1c-spin (round trip) | 22.86 | 23.01 / 22.92 | +1 / 0 % |
| ping-pong 1c-park | 22.85 | 22.82 / 22.99 | 0 / +1 % |
| ping-pong 2c-spin | 156.38 | 162.53 / 157.20 | +4 / +1 % |
| ping-pong 2c-park | 157.57 | 159.49 / 154.69 | +1 / −2 % |
| thread-ring 1c-spin (hop) | 17.21 | 17.08 / 17.02 | −1 / −1 % |
| thread-ring 1c-park | 17.15 | 17.13 / 17.00 | 0 / −1 % |
| thread-ring 2c-spin | 75.66 | 76.26 / 74.19 | +1 / −2 % |
| thread-ring 2c-park | 75.13 | 75.49 / 74.83 | 0 / 0 % |
| counting 1c-spin (message) | 7.53 | 7.51 / 7.52 | 0 / 0 % |
| counting 1c-park | 7.69 | 7.57 / 7.44 | −2 / −3 % |
| counting 2c-spin | 9.13 | 9.41 / 9.28 | +3 / +2 % |
| counting 2c-park | 9.31 | 9.28 / 9.10 | 0 / −2 % |
| chameneos 1c-spin (meeting) | 25.52 | 26.04 / 25.87 | +2 / +1 % |
| chameneos 1c-park | 25.86 | 25.90 / 25.63 | 0 / −1 % |
| chameneos 2c-spin | 43.61 | 43.53 / 44.12 | 0 / +1 % |
| chameneos 2c-park | 44.68 | 45.67 / 44.96 | +2 / +1 % |
| big 1c-spin (round trip) | 18.03 | 17.69 / 18.10 | −2 / 0 % |
| big 1c-park | 17.86 | 17.75 / 17.72 | −1 / −1 % |
| big 2c-spin | 17.90 | 17.59 / 17.59 | −2 / −2 % |
| big 2c-park | 18.32 | 17.51 / 18.31 | −4 / 0 % |
| bank-transaction 1c-spin (transfer) | 143.47 | 138.97 / 140.26 | −3 / −2 % |
| bank-transaction 1c-park | 143.00 | 138.66 / 138.87 | −3 / −3 % |
| bank-transaction 2c-spin | 81.10 | 80.19 / 83.47 | −1 / +3 % |
| bank-transaction 2c-park | 84.10 | 81.35 / 85.45 | −3 / +2 % |
| fork-join 1c-spin (message) | 7.32 | 7.55 / 7.53 | +3 / +3 % |
| fork-join 1c-park | 7.50 | 7.51 / 7.85 | 0 / +5 % |
| fork-join 2c-spin | 8.56 | 8.99 / 8.72 | +5 / +2 % |
| fork-join 2c-park | 8.45 | 8.63 / 8.43 | +2 / 0 % |

Reading: the four fib cells move by −19 to −35 % in both passes; the other 28 cells sit inside
±5 % and change sign between the two candidate passes wherever they reach it (ping-pong 2c-spin
+4 / +1, big 2c-park −4 / 0, bank-transaction 2c −1 / +3, fork-join 1c-park 0 / +5), which a
real effect does not do. fork-join reads +3 % on both 1c-spin passes — 0.2 ns on a 7.3 ns unit,
inside the ±3–5 % the field grids beside this one record for that cell.

## `dev/bench`, same session: the gate, the control, and the layout proof

The root `benchmarks` preset (`-march=native`, 582 TUs, 0 warnings) at `385bdb37`, then
`bench-run.sh --runs 3 --repetitions 5` over the 23 gated binaries, then `bench-compare.py`
against `dev/bench/baseline/linux-wsl2-x86_64.json` (recorded 2026-09-07 at qb `1ec7e794`):
**136 gated metrics — 107 within threshold, 24 improved beyond it, 5 regressed, VERDICT: FAIL.**
The 24 improvements are the train's work since that baseline (ask round trip +706 % same-core,
pipeline chain +122 %, mono ping-pong +78 %, the 8-core ping-pong +28 %…), not this branch's.
The 5 regressions were on two binaries, `qbm-http-bench-http1-parser` (`BM_Http1Parse_Fragmented`
+4.6–4.8 %, three metrics of one case) and `qb-io-bench-coroutine-pipeline`
(`BM_Stream_FilterMapReduce` +3.1–3.2 %, two metrics of one case) — neither of which constructs
an actor. Because a ten-day-old baseline is not a control, the flagged binaries plus two
actor-path anchors were **built at the control** (qb `f2779605`, same preset flags, a separate
tree) and run **alternated with the candidate three times** in the same quiet window
(`bench/bench-ab-compare.txt`): the actor anchors read ask round trips −0.2 / −0.9 %, mono
ping-pong −4.2 % (faster), multi ping-pong +1.2 %; the stream cell 0.0 %; but the parser's
fragmented case still read +5.8 %, the raw-thread reference ping-pong +4.0 % and the channel
cell +3.5 %, tight and non-overlapping across the three passes.

Then the proof (`bench/layout-proof*.log`). `cmp` on the binaries: **the control and candidate
`qbm-http-bench-http1-parser` are identical bytes, and so are the two `qb-io-bench-coroutine-
pipeline`** — the same program measured 5.8 % apart, which only the run can explain: the two
trees' executables have paths of different lengths (`~/prof/arena/ctl-bench/bin/benchmarks/` against
`~/qb-dev-seg/build/presets/benchmarks/bin/benchmarks/`), and the executable path is part of the
initial stack and environment layout every hot loop's alignment inherits (Mytkowicz, Diwan,
Hauswirth and Sweeney, "Producing wrong data without doing anything obviously wrong", ASPLOS
2009, where the environment size alone moved results by up to a third). Copied into ONE
directory under names of the same length, pinned, alternated (`bench/same*/`): the parser
405.1 vs 404.9 ns (5 processes each), the channel cell 55.67 vs 55.74 µs (7 each, overlap).
The third binary, `qb-core-bench-ping-pong-latency`, does contain the changed qb-core (its
`.text` differs in 550 KB of 639 KB — the actor code moved) and its reference cell is two raw
`std::thread`s over an SPSC ring, no actor at all: same-directory, seven alternated processes,
**200.9 vs 203.4 ms (+1.2 %, overlap)**; the two actor cells of that binary in the same run:
multi 190.5 vs 189.2 ms (−0.7 %, overlap), mono 59.0 vs 56.7 ms (**−4.0 %, separate**).

So the gate's FAIL is the baseline's age and the run's layout, not the branch: no gated metric
is slower than the control in the same session once the executable is the same bytes in the
same place, and the actor-path cells are faster. Two things follow for the maintainer, neither
of them this branch's to do: the WSL2 baseline is due for a re-record after the 3.2.0 train
(24 metrics beyond their improvement threshold), and `bench-run.sh` should be run from a fixed
executable path when it compares two trees.
