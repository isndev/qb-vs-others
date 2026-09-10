# qb branch `perf/loop-clock-on-demand` — Windows 11 / MSVC 19.51

The Windows half of the A/B for Huly **QB-180** (the WSL2 half, with the profile and the variant
censuses that shaped the fix, is `../../wsl-debian-g++14/qb-branch-perf-loop-clock-on-demand/`):
the qb branch that stops the core's tick phase from reading the wall clock on every pass when no
callback is registered, and reads the idle clock on every idle pass in every latency mode instead,
measured against the `develop` it forks from (`43f62afe`, the 3.2.0 candidate grid's build) on
`savina/ping-pong`, `savina/counting` and `savina/thread-ring`, 4 configurations each, plus the
four core `dev/bench` binaries. Same host, CPUs and flags as the published directories beside this
one: `/O2 /Ob2 /DNDEBUG`, CPUs 0,2, **9 repetitions + 2 warmup**, qb-only builds (`build/ab180-cand`
against the working tree, `build/ab180-ctl` against `D:\repo\qb-ctl-43f62afe`, a clean clone at
`43f62afe`, 0 dirty; shipped 3.1.0 is `build/shipped-win`), candidate / control / shipped /
candidate in ONE quiet session on 2026-09-07 with the WSL2 side idle (its own session had ended
10:11 UTC):

| directory | qb at | what |
|---|---|---|
| `grid-c42abddf/`, `grid-c42abddf-pass2/` | **the branch head `c42abddf`** — measured first and fourth | **12 cells** each, qb only, all verified: three shapes × {1c-spin, 1c-park, 2c-spin, 2c-park}. 10:17:17–10:18:35 UTC. |
| `grid-43f62afe/` | `develop` `43f62afe` — the control, measured second | same 12 cells, same session. |
| `grid-shipped-3.1.0/` | v3.1.0, measured third | same 12 cells, same session. |
| `bench/` | the four core `dev/bench` binaries, candidate and control alternated three times (`cand-N/` / `ctl-N/`, one process per run, 5 repetitions, every iteration recorded) | 10:18:35–10:22:18 UTC. |
| `census/` | `c42abddf` vs `43f62afe`, **10 interleaved launches** each, 3 reps + 1 warmup, on the four counting cells (the grid's pass 2 was elevated there), the two 2c-spin cells and the 1c ping-pong anchor | 10:22:31–10:23:15 UTC. |

None of the grids is merged into the published tables; the branch joins `qb-branch-develop/`
when the final candidate is measured on all eight shapes.

## The branch head against the control, same session (p50 per unit, ns)

| cell | shipped 3.1.0 | `43f62afe` | **`c42abddf`** p1 / p2 | Δ vs `43f62afe` | Δ vs shipped |
|---|---:|---:|---:|---:|---:|
| ping-pong 1c-spin (round trip) | 113.6 | 80.7 | **41.6 / 41.6** | **−48 %** | −63 % |
| ping-pong 1c-park | 113.1 | 81.5 | **42.8 / 41.3** | **−47 / −49 %** | −62 % |
| ping-pong 2c-spin | 348.9 | 251.7 | 267.1 / 245.5 | +6 / −3 % (bimodal, see the census) | −23 % |
| ping-pong 2c-park | 1 428.6 | 257.3 | 257.1 / 258.2 | 0 % | −82 % |
| thread-ring 1c-spin (hop) | 64.8 | 44.7 | **22.9 / 23.3** | **−49 / −48 %** | −65 % |
| thread-ring 1c-park | 64.6 | 45.5 | **23.0 / 23.3** | **−49 %** | −64 % |
| thread-ring 2c-spin | 159.7 | 123.3 | 123.3 / 124.7 | 0 / +1 % | −23 % |
| thread-ring 2c-park | 624.2 | 128.3 | 123.3 / 126.6 | −4 / −1 % | −80 % |
| counting 1c-spin (message) | 29.7 | 8.7 | 8.7 / 12.5 | 0 / +43 % (bimodal, see the census) | −71 % |
| counting 1c-park | 30.8 | 8.9 | 8.7 / 11.9 | −2 / +34 % (idem) | −72 % |
| counting 2c-spin | 33.5 | 11.4 | 11.2 / 13.8 | −2 / +21 % (idem) | −67 % |
| counting 2c-park | 33.1 | 13.4 | 11.0 / 14.0 | −18 / +4 % (idem) | −67 % |

The one-core cells halve on MSVC as they did on g++ (QueryPerformanceCounter is 19 ns here against
the vDSO's 13, so the per-pass read was a larger share of a longer pass). `census/`, ten
interleaved launches, medians (min … max): the four counting cells are **bimodal within a launch
for BOTH builds** — 1c-spin `43f62afe` 8.6 … 12.6 (9.0) against `c42abddf` 8.8 … 12.7 (9.5), 1c-park
9.0 … 12.8 (11.4) against 8.8 … 12.8 (10.9), 2c-spin 11.2 … 14.2 (11.4) against 10.9 … 16.5 (13.6),
2c-park 11.2 … 14.4 (13.4) against 11.0 … 14.6 (12.9) — the two modes §9.11 recorded for this host,
and the grid's pass 2 fell in the upper one; ping-pong 2c-spin **247.3** (223.0 … 272.4) against
**255.0** (224.3 … 261.1); thread-ring 2c-spin **122.4** (118.7 … 130.0) against **122.8** (119.3 …
127.8); ping-pong 1c-spin 80.7 against 41.9. Every cross-core and every counting pair overlaps — no
measurable difference by `FAIRNESS.md` §1.5 — which is what the idle-clock half of the fix is for:
on WSL2 the guard alone had cost the two 2c-spin cells +25 % (the other README), and Windows
measured the branch head only, with the clock read already in place.

## `dev/bench` — the four core binaries (median of three run medians, ns; `bench/`)

| cell | `43f62afe` | **`c42abddf`** | Δ |
|---|---:|---:|---:|
| `BM_Mono_PingPong_Latency` (same-core round trip) | 117.8 | **76.7** | **−35 %** |
| `BM_Multi_PingPong_Latency` (cross-core) | 323.4 | 285.9 | −12 % (306–335 against 265–307, three runs each) |
| `BM_Reference_Multi_PingPong_Latency` (raw spsc, the ratio's denominator) | 242.7 | 246.9 | +2 % |
| pipeline chain, 10 actors / 1 core (per delivery) | 79.1 | **43.3** | **−45 %** |
| pipeline chain, 8 actors / 8 cores | 254.9 | 257.1 | +1 % |
| `BM_PINGPONG<TinyEvent>` 64 actors, 1 core (per round trip) | 31.2 | 30.0 | −4 % |
| `BM_PINGPONG<TinyEvent>` 64 actors, 8 cores | 26.9 | 25.7 | −4 % |
| `BM_Ask_RoundTrip_SameCore` | 38.0 | 35.3 | −7 % |
| `BM_Ask_RoundTrip_CrossCore` | 46.3 | 43.6 | −6 % |

Suite at the head: `dev/agent/verify-windows.ps1` — the numbers are in the Huly comment and the
qb `CHANGELOG.md` `[Unreleased]` entry (release **373/373 executed, 0 skipped, 556 TUs, 0
warnings**; the other four presets in the same run).
