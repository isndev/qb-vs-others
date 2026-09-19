# qb branch `perf/default-event-registry` — Windows 11 / MSVC 19.51

The A/B for Huly **QB-174**: the qb branch that routes the five default events through the
actor registry, measured against the `develop` it forks from on the two Savina shapes that
create actors inside the window — `savina/fib` (57 312 actors born and dead per repetition,
`benchmarks/savina/fib.md`) and `savina/chameneos` (101 actors, 200 000 meetings through one
broker, `benchmarks/savina/chameneos.md`). Same host, CPUs and build flags as the published
directories beside this one: `/O2 /DNDEBUG`, CPUs 0 and 2 (two P-cores), **9 repetitions + 2
warmup**, qb-only builds for the candidate (`build/win-qb174`: `-DQVO_WITH_CAF=OFF
-DQVO_WITH_SOBJECTIZER=OFF -DQVO_WITH_BASELINE=OFF`, so the field is not re-measured here — it is
in `../qb-branch-perf-dense-table-growth/`), the three grids in ONE quiet session,
21:06:26–21:06:30 UTC on 2026-09-06, no build, no test suite and no WSL measurement running
anywhere on the host (the WSL2 grids ran 21:06:00–21:06:04 UTC and had ended). The candidate
was built against a git worktree of `qb/` at the branch; the control is `build/win-release`,
built against `qb/` at `e814df06`.

| directory | qb at | what |
|---|---|---|
| `grid-494d54a5/` | `perf/default-event-registry` = `develop` `e814df06` + **`494d54a5`** (the five default events dispatch through the actor registry, QB-174) — **the candidate**, measured FIRST | **8 cells**, qb only, all verified: fib × chameneos × {2c-spin, 2c-park, 1c-spin, 1c-park}. |
| `grid-e814df06/` | `develop` `e814df06` — **the control**, measured second, the full `qb/` tree at that commit | **8 cells**, same protocol, the same session. |
| `grid-494d54a5-pass2/` | `494d54a5` again, measured third | **8 cells**: the candidate's second pass, so a gain has to reproduce on both sides of the control before it is one. |

Candidate / control / candidate is the order, not control / candidate: a drift in the host over
the minute the three grids take would then show as the two candidate passes disagreeing, and
they do not. None of the three is merged into the published tables (`../savina-fib/`,
`../savina-chameneos/` render shipped 3.1.0); the candidate joins them when the 3.2.0 grid is
measured for all seven shapes in one session.

## Control against the candidate, same session (p50 ms)

| shape · config | `e814df06` | **`494d54a5`** | pass 2 | Δ (pass 1 / pass 2) |
|---|---|---|---|---|
| fib · 2c-spin | 10.18 | **7.10** | 7.03 | -30 % / -31 % |
| fib · 2c-park | 9.93 | **7.02** | 6.97 | -29 % / -30 % |
| fib · 1c-spin | 15.73 | **11.03** | 11.48 | -30 % / -27 % |
| fib · 1c-park | 15.78 | **11.15** | 11.24 | -29 % / -29 % |
| chameneos · 2c-spin | 13.37 | **12.57** | 12.22 | -6 % / -9 % |
| chameneos · 2c-park | 12.08 | **11.81** | 11.90 | -2 % / -1 % |
| chameneos · 1c-spin | 7.07 | **7.12** | 7.34 | +1 % / +4 % |
| chameneos · 1c-park | 7.12 | **6.91** | 7.14 | -3 % / +0 % |

fib is **−29 % to −31 % (2c 10.18 → 7.10 / 7.03 ms, 1c 15.73 → 11.03 / 11.48 ms)** — every one of its four cells moves by more than its p99–p50 spread and
the two candidate passes agree to within 4 %. Chameneos does not move (2c 13.37 → 12.57 / 12.22 and 12.08 → 11.81 / 11.90 ms, inside the 2c-spin cell's own 2.9 ms IQR at the control; 1c 7.07 → 7.12 / 7.34): it
creates 101 actors and pushes 400 000 events through a broker, and neither the five `key_table`
inserts per actor lifetime nor the resolver walk at removal is on that path. That is the shape of
the change — an actor's construction and destruction, nothing per message.

What `494d54a5` does (qb `CHANGELOG.md`, `[Unreleased]`): the five default events
(`KillEvent`, `SignalEvent`, `UnregisterCallbackEvent`, `PingEvent`, `RequireEvent`) no longer
have a per-type handler table each — 65 536 × 32-byte slots per core per event, whose key set was
exactly the set of live actors, which the per-core `ActorMap` already is. `VirtualCore` installs
one `DefaultEventResolver<E>` per default event into the router at construction; a unicast is the
registry's own `__actor_slot__(dest)` lookup and then the actor's dispatch pointer
(`Actor::_default_on[k]`), a broadcast walks the registry. `registerEvent<E>` stores a trampoline
pointer instead of inserting into a table; `unregisterEvents()` no longer visits a resolver that
owns nothing. The `perf` profile at `001be013` had put those five inserts at ≈ 29 % of an actor's
lifetime and the resolver walk at ≈ 12 % (`../qb-branch-perf-dense-table-growth/README.md`);
the measured −30 % at two cores and −30 % at one on fib is that share, less the registry lookup the unicast now pays.
