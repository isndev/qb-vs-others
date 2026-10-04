# qb `QB-65` — one generation per signal (WSL2 Debian 13, g++-14, i9-12900K)

Control `9dfe1e08` against candidate `72f1fb5f` (committed as `fa925cc8`: the same tree, its message amended to record
these figures; the trees are `git archive` of the two commits; the candidate's four
signal files are asserted byte-identical to the commit before the build, `scripts/wsl-rebuild-cand.sh`). qb adapter
only, `--cpus 0,2`, quiet host, nothing running on Windows.

## Verdict — `copies/`: 4 physical copies per side, seeded random order per round, 40 rounds

| cell / probe | candidate vs control (median of copy medians) | copy spread, control / candidate |
|---|---|---|
| ping-pong 1c spin | −0.3 % | 0.4 % / 0.4 % |
| thread-ring 1c spin | −1.8 % | 0.3 % / 0.4 % |
| ping-pong 2c spin | −0.6 % | 1.7 % / 1.1 % |
| thread-ring 2c spin | +1.4 % | 0.6 % / 1.6 % |
| pass-cost k = 1 | +0.1 % | |
| ask-cost | −0.9 % | |

Level everywhere but thread-ring 2c spin (+1.4 %, the copies separate), which is the idle-poll pacing effect the
Windows directory isolates (`../../desktop-win11-msvc19/qb-branch-qb65-signal-generations/`, `s2/`): the two-core
spin exchange gets slower when the idle pass gets faster (`VirtualCore.cpp`, Huly QB-180).

## Why several copies

A single-copy run on this host (random order, 100 rounds, an A/A pair of byte-identical binaries in two files) put
the A/A pair 3.4 % apart on ping-pong 2c: each file has its own physical pages, and the hot code's place in the
physically indexed caches follows them. One copy per side cannot resolve a 2c delta below that; the median of
several copies can. An earlier measurement here was also taken on an intermediate cut of the change rather than the
commit, and was discarded. The per-run JSON files are in `raw.tar.xz`; the scripts are in `scripts/`.
