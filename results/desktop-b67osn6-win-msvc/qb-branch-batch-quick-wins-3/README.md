# The quick-win batch — release neutrality on Windows 11 / MSVC 19.51

The Windows half of `../../wsl-debian-g++14/qb-branch-batch-quick-wins-3/` (the batch and the
layout lesson are described there): qb `batch/quick-wins-3` `b29602bd` (QB-62 event-driven
`ready_async`, QB-61 width `static_assert`, QB-84 abandoned-frame report, QB-66 doc) against
`develop` `ef89f175`, both built from the same qb-vs-others tree by `cl` 19.51.36256, measured
2026-09-09 **04:55–04:57 UTC** in one quiet session (no build during the points, Docker Desktop
quit, the WSL2 side idle at 0.01): six Savina cells × eight interleaved rounds (3 repetitions +
1 warm-up, CPUs 0,2), three probes × five alternations (1.5 s, CPU 0).

| file | what |
|---|---|
| `census/` | the six cells × ctl/cand × 8 rounds (JSON) + `census-summary.txt` |
| `probe.txt`, `probe-summary.txt` | `ask-cost` `push` / `ask`, `pass-cost` k = 1 — ctl/cand × 5 |

## The cells (`work_p50 / work_units`, ns, medians of eight rounds)

| cell | ctl `ef89f175` | cand `b29602bd` | Δ | the eight rounds ctl / cand |
|---|---:|---:|---:|---|
| bank-transaction 1c | 235.2 | 233.2 | −0.9 % | 229.5–263.4 / 229.4–241.4 |
| big 1c | 17.4 | 17.4 | level | 16.8–19.5 / 16.8–20.3 |
| counting 1c | 8.6 | 11.0 | bimodal on both | 8.0–12.1 / 8.1–12.3 (the two modes, 8 and 12, land where they will) |
| fib 1c | 179.2 | 181.7 | +1.4 % | 174.7–186.1 / 177.4–206.9 |
| ping-pong 1c | 31.4 | 30.7 | −2.2 % | 30.8–32.4 / 30.1–31.3 |
| ping-pong 2c | 188.9 | 185.4 | −1.9 % | 180.1–210.9 / 178.4–195.4 |

## The probes (one core, medians of five)

| probe | ctl | cand | Δ |
|---|---:|---:|---:|
| `push` (ns per trip) | 31.38 | 31.26 | −0.4 % |
| `ask` (500 ms timeout) | 90.71 | 90.05 | −0.7 % |
| `pass-cost` k = 1 (ns per pass) | 15.54 | 15.59 | +0.3 % |

Level on every cell whose spread is unimodal; `counting` 1c is the bimodal cell §9.12 already
records on MSVC (8 / 12 ns modes on both binaries), and its medians say which mode the majority
of eight rounds fell in, not a difference between the binaries.
