# qb `perf/fjcreate-regression` — the fork-join-create regression of the 3.3.0 candidate, WSL2 / Debian 13 / g++ 14.2

The A/B for Huly QB-1009: `savina/fork-join-create` at one core was 19–25 % slower with qb
`develop` `73018675` (the 3.3.0 candidate) than with shipped v3.2.1 on WSL2 / g++ (wave A,
`../qb-branch-develop/README.md`, `docs/TUNING.md` §21.3), level on Windows. Three builds of one
harness tree, qb cells only, all four configurations, 9 repetitions + 2 warmup, CPUs 0 and 2, in
ONE quiet session: the candidate, the fix, shipped 3.2.1, the fix again, the candidate again, then
two interleaved launch censuses on the one-core cells (12 launches of 3 + 1 per build, AB/BA
order). 20 grid cells and 96 census launches, all verified. Not rendered by `report.py`.

**The chain.** v3.2.1 (`82ac0531`) → `0e818396` (on `develop`, the first bad commit by bisection:
it removed the `owned_current_` thread-local) → `73018675` (the candidate) → the fix, branch
`perf/fjcreate-regression`, three commits on `73018675` (`840065b0` the change, `8674749a` its unit
case, `4b6a4300` the API reference). **Measured at `4b6a4300` (on `73018675`); lands on qb
`develop` as `4ef5270f` (`a1f5e087`, `14a11d23`, `4ef5270f`, rebased over two test-only commits),
`src/` identical.**

**The mechanism, in two lines.** At every core thread's exit, `thread_arena`'s reaper freed the
thread's 64 KiB first chunk with `::operator delete`; since `0e818396` that chunk sits right below
glibc's arena top, the merged top crossed the dynamic trim threshold and was `madvise`d away
(4 MB), so the next engine's core thread re-faulted ~770 pages while it created its actors, inside
the window. The fix hands a thread's first chunk to a process-wide spare list
(`thread_arena::spare()`) that the next thread's first refill takes: no free at exit, no trim, a
warm chunk.

**The session.** 2026-10-11, **03:42:12–03:43:25 UTC** (60 s of quiet, then the five grids 03:43:12–03:43:13, the two censuses 03:43:13–03:43:15; the same session then re-measured the logmap row and its SObjectizer sweep, 03:43:15–03:43:25, `../savina-logmap/` and `../wave-a-form-sweep/`), the Windows side idle (its own session ran 03:43:51–03:45:13). Builds: `~/wa/b-cnd` (qb `73018675`), `~/wa/b-fix` (qb `4b6a4300`) and `~/wa/b-shp` (shipped v3.2.1 `82ac0531`, the field build): three `git archive`s built apart through the same adapters (qb-vs-others `ef2d3f8c`), g++ 14.2.0 `-O3 -DNDEBUG`, 0 warnings, the three executables at paths of one length.

ns per actor, p50 [min–p99]; in brackets the delta to shipped 3.2.1, "sep" where the
distributions do not overlap (`grid-*/`, in the order they ran):

| config | candidate `73018675` | fix `4b6a4300` | shipped 3.2.1 | fix, pass 2 | candidate, pass 2 |
|---|---|---|---|---|---|
| 1c-spin | 76.6 [74.4–84.2] (+25.1 %, sep) | 63.9 [59.5–66.5] (+4.3 %) | **61.2 [58.7–70.9]** | 62.8 [58.9–66.1] (+2.6 %) | 76.6 [74.9–83.6] (+25.1 %, sep) |
| 1c-park | 75.9 [73.9–79.9] (+24.4 %, sep) | 62.8 [60.4–63.3] (+3.0 %) | **61.0 [57.4–65.7]** | 63.3 [60.0–64.5] (+3.7 %) | 74.9 [73.9–78.9] (+22.7 %, sep) |
| 2c-spin | 33.1 [30.4–38.5] (+11.6 %) | 30.5 [29.7–39.5] (+2.8 %) | **29.6 [29.1–34.1]** | 34.6 [29.3–41.3] (+16.7 %) | 30.6 [28.8–38.0] (+3.3 %) |
| 2c-park | 30.7 [29.5–36.3] (+1.6 %) | 30.9 [29.2–33.0] (+1.9 %) | **30.3 [28.8–38.4]** | 32.0 [29.9–34.3] (+5.8 %) | 32.1 [29.8–34.5] (+6.1 %) |

The censuses, median of the twelve launch medians [min–max], ns per actor:

| census | config | the other build | fix `4b6a4300` | Δ | distributions |
|---|---|---|---|---|---|
| `census-4b6a4300-vs-73018675/` | 1c-spin | 75.5 [73.2–77.5] | 62.7 [59.2–67.5] | -17.0 % | **separate** |
| `census-4b6a4300-vs-73018675/` | 1c-park | 74.7 [72.7–76.8] | 64.2 [59.1–72.2] | -14.1 % | **separate** |
| `census-4b6a4300-vs-shipped-3.2.1/` | 1c-spin | 64.9 [60.6–73.6] | 64.6 [62.4–71.1] | -0.4 % | overlap |
| `census-4b6a4300-vs-shipped-3.2.1/` | 1c-park | 62.2 [60.5–64.5] | 62.9 [58.7–65.6] | +1.1 % | overlap |

**The fix brings the one-core cells back to 3.2.1's level.** At one core the candidate is
22.7–25.1 % slower than shipped 3.2.1 in both of its passes, every distribution separate, as in
the wave-A session (61.2 → 74.4 by census there); the fix is 2.6–4.3 % above shipped in its two
passes, every distribution overlapping, and the census puts it 14–17 % under the candidate
(separate) and level with 3.2.1 (−0.4 / +1.1 %, overlapping). The bisect's A/B read 61.1 / 76.7 /
62.8 ns at 1c-park for shipped / candidate / fix; this session reads 61.0 / 75.9 and 74.9 / 62.8
and 63.3. The two-core cells are level for all three builds (their p99 reaches 34–41 ns on every
build at 2c-spin).

`session.log` has the legs, the counts asserted (4 cells per grid, 48 launches per census, all
verified) and the host's state at both ends; each `grid-*/run.log` and `census-*/census.log` is the
tool's own output. The wave-A field keeps shipped 3.2.1 as its qb column, and
`../qb-branch-develop/grid-73018675/` stays as measured at `73018675`: this directory is the
evidence that the fix closes the one shape the candidate lost on.
