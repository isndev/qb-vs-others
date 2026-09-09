# The quick-win batch — release neutrality on WSL2 Debian 13 g++ 14.2

qb `batch/quick-wins-3` = `develop` `bb5b3e67` + four branches, none of which touches a hot path:
QB-62 (`ActorHandle::ready_async` event-driven: an intrusive waiter list per Activating entry,
fired by the pass that ends the activation, instead of a 1 ms poll), QB-61 (an event type wider
than the mailbox ring refused at compile time), QB-84 (abandoned coroutine frames reported in
every build and tallied), QB-66 (a doc cue). The rule is to measure anyway, and the measurement
found something worth keeping: a **layout effect of +2 %** on the tightest cell, from where two
cold functions had been placed.

Protocol (`FAIRNESS.md`): ctl and cand built from `git archive` in the same session, 60 s of quiet
after the builds, one quiet host (Windows side idle, Docker Desktop quit), cells interleaved
cand/ctl per round, 3 repetitions + 1 warm-up, CPUs 0,2; probes 2 s on CPU 2.

| file | what |
|---|---|
| `census/` | the final candidate `b29602bd`: six cells × ctl/cand × 8 interleaved rounds (JSON) + `census-summary.txt` |
| `probe.txt`, `probe-summary.txt` | `ask-cost` `ask` / `push`, `pass-cost` k = 1 — ctl/cand × 5 |
| `census-pp/` | the second look at ping-pong 1c: 12 rounds, ctl first, the union `bafa2216` and each branch ALONE (`b62` `a72c23cc`, `b61` `96834a14`, `b84` `76875143`) |
| `census-pp2/` | the same after the relocation: ctl, QB-62 alone with its cold bodies at the file ends (`ce8f42b7`), the re-merged union `b29602bd` |

## The final candidate (`b29602bd`, `work_p50 / work_units`, ns, medians of eight rounds)

| cell | ctl `bb5b3e67` | cand `b29602bd` | Δ | quartiles ctl / cand |
|---|---:|---:|---:|---|
| bank-transaction 1c | 144.1 | 142.8 | −0.9 % | 141.0–145.1 / 140.8–144.8 |
| big 1c | 18.2 | 18.0 | −0.9 % | 18.0–18.2 / 17.6–18.1 |
| counting 1c | 7.6 | 7.5 | −1.0 % | 7.4–7.7 / 7.4–7.8 |
| fib 1c | 131.1 | 127.0 | −3.1 % | 126.3–133.5 / 125.4–127.4 |
| ping-pong 1c | 22.6 | 22.9 | +1.2 % | 22.5–22.9 / 22.5–23.1 |
| ping-pong 2c | 154.9 | 156.8 | +1.2 % | 153.1–156.0 / 153.9–157.6 |

| probe (one core, medians of five) | ctl | cand | Δ |
|---|---:|---:|---:|
| `ask` (round trip) | 46.89 | 46.08 | −1.7 % |
| `push` (round trip) | 24.20 | 23.40 | −3.3 % |
| `pass-cost` k = 1 (ns per pass) | 12.47 | 12.54 | +0.6 % |

Every cell's quartiles overlap: **level**.

## The layout effect, found and removed

The first census (union `bafa2216`) read ping-pong 1c **+2.1 %** with the quartiles SEPARATED
(22.1–22.5 / 22.6–22.8) on a batch whose every change is off the ping-pong path. The second
look (`census-pp/`, 12 rounds, ctl first) attributed it: the union +0.9 % (overlapping), QB-61
alone +0.7 %, QB-84 alone +1.3 %, **QB-62 alone +2.2 % with separated quartiles**
(22.79–23.06 against 22.38–22.79). Nothing in QB-62 runs on that path — the pump's early-out is
unchanged, the waiter functions are called only by `ready_async` — but its two cold bodies had
been placed BEFORE the hot ones: `__fire_activation_waiters__` ahead of `__pump_activations__` and
`__workflow__` in VirtualCore.cpp, `activation_wait` / `activation_unwait` ahead of `ask_take` and
`Actor::getPipe` in Actor.cpp, so every address after them moved and the pass's code alignment
with it. Moved to the END of their files (`ce8f42b7`), the same source reads **+0.1 %** alone and
**+0.1 %** in the re-merged union (`census-pp2/`: 22.63 / 22.66 / 22.66, quartiles overlapping),
and the final census above is what ships. The rule it adds to the ones TUNING §18 already carries:
a cold body added to a hot translation unit goes AFTER the hot ones, and the census decides,
not the reasoning that "it is never called".
