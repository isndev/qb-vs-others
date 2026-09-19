# QB-63 — the engine's footprint: what N cores hold at rest, after the first traffic, and after they stop

`tools/probes/pipe-footprint.cpp` (`qvoprobe-pipe-footprint`), qb `develop` `c15d9d9d`, 2026-09-09, one
run per cell: N ∈ {4, 8, 16, 32, 64, 128} cores, one actor each, `latency` 100 µs, three modes — `idle`
(no event), `broadcast` (core 0's actor broadcasts once: N pipes used), `mesh` (every actor pushes one
event to every other: N × (N − 1) pipes used). Four readings per run, resident set and private commit
(Linux: `VmRSS` / `VmHWM` — the second is a PEAK and never falls; Windows: `WorkingSetSize` /
`PrivateUsage`), each as a delta over the process's baseline and beside the model the reading is
compared against: `N² × 64 KiB` of mailbox rings plus 256 KiB per pipe that carried an event. The four
phases: `baseline` (before `Main::start()`), `settled` (the engine idle after the traffic), `stopped`
(`Main::stop()` + `join()`), `destroyed` (the `Main` object gone). `docs/TUNING.md` §20 is the reading
guide; `qb/readme/0_foundations/buffers.md` carries the table.

| file | what |
|---|---|
| `pipe-footprint.txt` | the 18 cells × 4 phases. The resting cost is the rings, resident from start (128 cores: 1.02 GiB on both hosts, within 4 % of the model); the traffic cost is the segments (128-core mesh: +4.0 GiB — resident on Linux, where a slab is populated when mapped, committed-not-resident on Windows until events land); what stays after the engine is destroyed is the slab cache (4.0 GiB at 128 mesh on both, mapped and warm for the next engine — `slab_cache::trim()` returns it). |

Not merged into the published tables: a qb-only instrument.
