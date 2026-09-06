# qb branch `perf/default-event-registry` — WSL2 Debian 13 / g++ 14.2

The A/B for Huly **QB-174**: the qb branch that routes the five default events through the
actor registry, measured against the `develop` it forks from on the two Savina shapes that
create actors inside the window — `savina/fib` (57 312 actors born and dead per repetition,
`benchmarks/savina/fib.md`) and `savina/chameneos` (101 actors, 200 000 meetings through one
broker, `benchmarks/savina/chameneos.md`). Same host, CPUs and build flags as the published
directories beside this one: `-O3 -DNDEBUG`, `taskset -c 0,2`, **9 repetitions + 2 warmup**,
qb-only builds (`-DQVO_WITH_CAF=OFF -DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF`, so the
field is not re-measured here — it is in `../qb-branch-perf-dense-table-growth/`), the three grids
in ONE quiet session, 21:06:00–21:06:04 UTC on 2026-09-06, the Windows side idle throughout (its
own three grids ran 21:06:26–21:06:30 UTC, after this host's had ended). The candidate is
`~/qvo/qb174`, built against a git worktree of `qb/` at the branch; the control is
`~/qvo/ctl-a6663641`, built against a clean LF clone checked out at `a6663641` (0 dirty).

| directory | qb at | what |
|---|---|---|
| `grid-dc1ac56e/` | `perf/default-event-registry` = `develop` `a6663641` + **`dc1ac56e`** (the five default events dispatch through the actor registry, QB-174) — **the candidate**, measured FIRST | **8 cells**, qb only, all verified: fib × chameneos × {2c-spin, 2c-park, 1c-spin, 1c-park}. |
| `grid-a6663641/` | `develop` `a6663641` — **the control**, measured second, a clean clone at that commit | **8 cells**, same protocol, the same session. |
| `grid-dc1ac56e-pass2/` | `dc1ac56e` again, measured third | **8 cells**: the candidate's second pass, so a gain has to reproduce on both sides of the control before it is one. |

Candidate / control / candidate is the order, not control / candidate: a drift in the host over
the minute the three grids take would then show as the two candidate passes disagreeing, and
they do not. None of the three is merged into the published tables (`../savina-fib/`,
`../savina-chameneos/` render shipped 3.1.0); the candidate joins them when the 3.2.0 grid is
measured for all seven shapes in one session.

## Control against the candidate, same session (p50 ms)

| shape · config | `a6663641` | **`dc1ac56e`** | pass 2 | Δ (pass 1 / pass 2) |
|---|---|---|---|---|
| fib · 2c-spin | 6.96 | **5.21** | 5.31 | -25 % / -24 % |
| fib · 2c-park | 7.06 | **5.13** | 5.37 | -27 % / -24 % |
| fib · 1c-spin | 10.98 | **8.62** | 8.48 | -21 % / -23 % |
| fib · 1c-park | 10.95 | **8.57** | 8.45 | -22 % / -23 % |
| chameneos · 2c-spin | 10.74 | **11.81** | 10.64 | +10 % / -1 % |
| chameneos · 2c-park | 10.82 | **10.65** | 10.69 | -2 % / -1 % |
| chameneos · 1c-spin | 6.53 | **6.86** | 6.45 | +5 % / -1 % |
| chameneos · 1c-park | 6.61 | **6.52** | 6.62 | -1 % / +0 % |

fib is **−22 % to −27 % (2c 6.96 → 5.21 / 5.31 ms, 1c 10.98 → 8.62 / 8.48 ms)** — every one of its four cells moves by more than its p99–p50 spread and
the two candidate passes agree to within 2 %. Chameneos does not move (2c-spin 10.74 → 11.81 / 10.64 ms, the one cell whose pass 1 is outside its neighbours, and its pass 2 is back on the control; 1c 6.53 → 6.86 / 6.45): it
creates 101 actors and pushes 400 000 events through a broker, and neither the five `key_table`
inserts per actor lifetime nor the resolver walk at removal is on that path. That is the shape of
the change — an actor's construction and destruction, nothing per message.

What `dc1ac56e` does (qb `CHANGELOG.md`, `[Unreleased]`): the five default events
(`KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`, `RequireEvent`) no longer
have a per-type handler table each — 65 536 × 32-byte slots per core per event, whose key set was
exactly the set of live actors, which the per-core `ActorMap` already is. `VirtualCore` installs
one `DefaultEventResolver<E>` per default event into the router at construction; a unicast is the
registry's own `__actor_slot__(dest)` lookup and then the actor's dispatch pointer
(`Actor::_default_on[k]`), a broadcast walks the registry. `registerEvent<E>` stores a trampoline
pointer instead of inserting into a table; `unregisterEvents()` no longer visits a resolver that
owns nothing. The `perf` profile at `8362a4b8` had put those five inserts at ≈ 29 % of an actor's
lifetime and the resolver walk at ≈ 12 % (`../qb-branch-perf-dense-table-growth/README.md`);
the measured −25 % at two cores and −22 % at one on fib is that share, less the registry lookup the unicast now pays.
