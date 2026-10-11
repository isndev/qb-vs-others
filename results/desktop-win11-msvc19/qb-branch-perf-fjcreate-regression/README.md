# qb `perf/fjcreate-regression` — the fork-join-create regression of the 3.3.0 candidate, Windows 11 / MSVC 19.51

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

**The session.** 2026-10-11, **03:43:51–03:45:13 UTC** (60 s of quiet, then the five grids 03:44:51–03:44:54, the two censuses 03:44:54–03:44:58; the same session then re-measured the logmap row and its SObjectizer sweep, 03:44:58–03:45:13, `../savina-logmap/` and `../wave-a-form-sweep/`), after the WSL2 session had ended at 03:43:25; `\Processor(_Total)` 0.2–1.7 % before, 1.2–2.3 % after. Builds: `build/wa-cnd` (qb `73018675`), `build/wa-fix` (qb `4b6a4300`) and `build/wa-shp` (shipped v3.2.1 `82ac0531`, the field build): three `git archive`s built apart through the same adapters (qb-vs-others `ef2d3f8c`), MSVC 19.51.36256 `/O2 /Ob2 /DNDEBUG`, 0 warnings, the three executables at paths of one length.

ns per actor, p50 [min–p99]; in brackets the delta to shipped 3.2.1, "sep" where the
distributions do not overlap (`grid-*/`, in the order they ran):

| config | candidate `73018675` | fix `4b6a4300` | shipped 3.2.1 | fix, pass 2 | candidate, pass 2 |
|---|---|---|---|---|---|
| 1c-spin | 100.6 [96.9–112.7] (-1.9 %) | 105.6 [98.7–108.9] (+3.1 %) | **102.5 [98.3–107.0]** | 102.0 [99.7–119.5] (-0.5 %) | 103.5 [102.3–107.4] (+1.0 %) |
| 1c-park | 103.6 [101.9–119.0] (+1.8 %) | 99.8 [98.2–103.9] (-1.9 %) | **101.8 [100.7–105.9]** | 100.2 [96.2–102.7] (-1.6 %) | 102.2 [100.6–111.4] (+0.4 %) |
| 2c-spin | 59.4 [57.6–70.6] (+0.5 %) | 58.9 [55.0–68.5] (-0.3 %) | **59.1 [55.0–70.7]** | 57.1 [54.2–59.8] (-3.3 %) | 59.2 [56.1–65.8] (+0.3 %) |
| 2c-park | 59.1 [55.5–68.1] (+9.0 %) | 55.2 [53.6–102.2] (+1.8 %) | **54.2 [52.1–56.2]** | 57.2 [56.2–67.7] (+5.6 %) | 55.1 [53.1–60.1] (+1.6 %) |

The censuses, median of the twelve launch medians [min–max], ns per actor:

| census | config | the other build | fix `4b6a4300` | Δ | distributions |
|---|---|---|---|---|---|
| `census-4b6a4300-vs-73018675/` | 1c-spin | 104.0 [97.9–112.7] | 101.8 [99.5–105.8] | -2.1 % | overlap |
| `census-4b6a4300-vs-73018675/` | 1c-park | 103.0 [97.2–107.7] | 100.7 [98.2–109.2] | -2.3 % | overlap |
| `census-4b6a4300-vs-shipped-3.2.1/` | 1c-spin | 103.9 [98.2–111.7] | 103.4 [99.2–112.6] | -0.5 % | overlap |
| `census-4b6a4300-vs-shipped-3.2.1/` | 1c-park | 103.9 [99.3–109.0] | 103.7 [99.0–107.4] | -0.3 % | overlap |

**Level for all three builds, as expected.** The mechanism found on WSL2 is glibc's arena trim; on
this host the wave-A session already read the candidate level with 3.2.1 (census −0.9 / +1.3 %),
and this session agrees: every grid cell of the three builds
within −3.3 to +9.0 % of shipped with the distributions overlapping, and both censuses level
(fix against the candidate −2.1 / −2.3 %, against 3.2.1 −0.5 / −0.3 %, overlapping).

`session.log` has the legs, the counts asserted (4 cells per grid, 48 launches per census, all
verified) and the host's state at both ends; each `grid-*/run.log` and `census-*/census.log` is the
tool's own output. The wave-A field keeps shipped 3.2.1 as its qb column, and
`../qb-branch-develop/grid-73018675/` stays as measured at `73018675`: this directory is the
evidence that the fix closes the one shape the candidate lost on.
