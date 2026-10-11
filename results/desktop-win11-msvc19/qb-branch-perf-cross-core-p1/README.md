# qb-branch-perf-cross-core-p1 — Windows MSVC 19.51 (2026-10-11)

The Windows half of the receipts of Huly QB-979 (the flush's stall rule) and QB-1010 (cigsmok's two-core
placement): the same reserved session as the WSL2 half (`idle-record.txt`: the CPU before the timing), the
Windows host of the i9-12900K, CPUs 0 and 2, MSVC 19.51 `/O2 /Ob2 /DNDEBUG`, the qb-vs-others tree at
`622ad51f`. Read with `docs/TUNING.md` §21.7 and §21.8 and the WSL2 directory's README for the protocol.

Only the rule's final form was measured here (the two refused forms were refused on WSL2 first): control qb
`9aebf2fc` against qb branch `perf/cross-core-p1`.

| directory | what |
|---|---|
| `cells/` | `summary.txt`, `orders.txt`, `meta.txt` of the cells run (`ab-qvo-cells-win.ps1`, the WSL2 runner's twin: 3 copies a side, seeded order, 8 rounds of 3 + 1, 60 s quiet) |
| `bench/` | `dev/bench/ab.py` over the four event-path binaries, the production flags (MSVC has no `-falign-*`; a cell that moves goes to the nop lottery of §9): `summary.txt` the tool's table, `ab-meta.json` |
| `probe/` | the cigsmok arbiter probe (§21.7 instrument 4): the wait per round by core pair, four repetitions, and its result document |
| `barber-remedy/` | barber 2c-spin re-read on the rule's shipped form (the refused-event branch out of line in a cold `__refused__`): −29.2 % per haircut, copy spreads 0.4 / 0.3 % (`summary.txt`, `orders.txt`); the rule's own form read −29.5 / −29.2 % in `cells/` |

The per-run result documents stayed in the session directory (`D:\repo\p1`).
