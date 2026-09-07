# qb `develop` `f8eba11d` — the 3.2.0 candidate grid, WSL2 / Debian 13 / g++ 14.2

The WSL2 half of the **3.2.0 candidate grid** (the Windows half is
`../../desktop-b67osn6-win-msvc/qb-branch-develop/`): qb `develop` at **`f8eba11d`** — 29
commits over the shipped v3.1.0 (`eac739ff`), every perf branch of `docs/TUNING.md` §7–§12
merged (axes A–N, QB-43 the segmented pipe, axis I the dense router, QB-174 the default-event
registry, the dense-table-growth chain fib produced, the five ask-path fixes bank-transaction
produced and QB-178 the ask slot table) — measured through the unmodified adapters on **all
eight Savina shapes**, 32 cells, against shipped 3.1.0 in ONE quiet session. Same guest, vCPUs
and build flags as the published directories beside this one: g++ 14.2.0 `-O3 -DNDEBUG`, vCPUs
0 and 2, **9 repetitions + 2 warmup** (the field is 5 + 1; the qb columns of a candidate grid
are 9 + 2 on both hosts), candidate / control / candidate on 2026-09-07,
**08:45:23–08:54:36 UTC**, the Windows side idle throughout (its own session ended 08:26:18
UTC, no build and no test suite running on either side). The candidate is `~/qvo/linux` — the
full field build whose `QVO_QB_DIR` is this superproject's qb submodule, the same tree that
measured CAF, SObjectizer and the floor of the eight `savina-*/` directories — rebuilt at
`f8eba11d` (0 dirty, 08:28 UTC); the control is `~/qvo/shipped`, built from `~/qb-head`, the
v3.1.0 source tree (`QB_FRAMEWORK_VERSION "3.1.0"`, frameworks off). Both harnesses ran with
`$HOME` (ext4) as their working directory: shipped 3.1.0's fib writes a ~61 MB `qb.1.log` per
repetition, which is what makes the control leg 8 min 40 s of a 9-minute session, and which
on a 9p mount would have been the measurement.

| directory | when (UTC) | what |
|---|---|---|
| `grid-f8eba11d/` | 08:45:23–08:45:39 | **the candidate**, 32 cells, 32 / 32 verified — README.md's WSL2 `framework=qb` grid. |
| `grid-shipped-3.1.0/` | 08:45:39–08:54:19 | **the control**, v3.1.0 through the same adapters, same session, 32 / 32 verified. |
| `grid-f8eba11d-pass2/` | 08:54:19–08:54:36 | the candidate again, after the control — the repeatability column below, 32 / 32 verified. |

This is the grid that **joins the tables**: README.md's candidate grids for Windows and WSL2
render `grid-f8eba11d/`, and `docs/TUNING.md` §13 reads the two hosts side by side. Every
earlier branch directory beside this one (`qb-branch-perf-*/`) is the A/B that produced one of
the 29 commits and stays as its provenance.

## Shipped 3.1.0 against the candidate, same session (ns per unit, p50)

The field columns are the published `../savina-<shape>/` cells (2026-09-04, -06 and -07, each
its own quiet session, 5 + 1); the three qb columns are this session.

| shape (per unit) | config | shipped 3.1.0 | **`f8eba11d`** | pass 2 | Δ (p50) | min: 3.1.0 / cand / pass 2 | CAF / SObjectizer / floor |
|---|---|---|---|---|---|---|---|
| `ping-pong` (round trip) | 1c-spin | 100.3 | **66.0** | 65.9 | -34.2 % / -34.3 % | 99.1 / 65.2 / 64.9 | 283.7 / 145.1 / 1.6 |
|  | 1c-park | 99.8 | **66.4** | 66.5 | -33.4 % / -33.4 % | 99.2 / 65.7 / 66.0 | 276.2 / 166.2 / 1.6 |
|  | 2c-spin | 290.9 | **222.0** | 234.3 | -23.7 % / -19.4 % | 273.1 / 207.2 / 224.4 | 297.7 / 629.8 / 210.1 |
|  | 2c-park | 29,026.5 | **227.0** | 247.0 | -99.2 % / -99.1 % | 28,627.2 / 213.5 / 232.8 | 290.0 / 26,525.2 / 25,467.4 |
| `counting` (message) | 1c-spin | 43.6 | **8.7** | 9.2 | -80.2 % / -78.9 % | 43.2 / 8.6 / 8.6 | 116.2 / 108.0 / 2.8 |
|  | 1c-park | 43.9 | **8.5** | 9.4 | -80.6 % / -78.6 % | 43.4 / 8.4 / 8.5 | 117.5 / 109.6 / 6.5 |
|  | 2c-spin | 47.9 | **11.0** | 11.0 | -77.1 % / -77.0 % | 47.7 / 10.3 / 10.6 | 170.8 / 162.8 / 23.7 |
|  | 2c-park | 48.4 | **11.0** | 11.0 | -77.3 % / -77.3 % | 47.3 / 10.5 / 10.8 | 167.0 / 172.5 / 59.8 |
| `thread-ring` (hop) | 1c-spin | 55.1 | **39.3** | 38.7 | -28.7 % / -29.8 % | 54.2 / 38.4 / 37.8 | 139.6 / 70.9 / 2.9 |
|  | 1c-park | 55.8 | **38.7** | 39.3 | -30.6 % / -29.6 % | 55.4 / 38.1 / 38.4 | 139.8 / 82.5 / 12.9 |
|  | 2c-spin | 169.1 | **112.3** | 127.6 | -33.6 % / -24.5 % | 159.4 / 107.2 / 125.0 | 140.4 / 304.4 / 114.4 |
|  | 2c-park | 14,541.5 | **120.7** | 128.7 | -99.2 % / -99.1 % | 14,173.5 / 115.1 / 125.7 | 141.4 / 264.6 / 13,008.2 |
| `fork-join` (message) | 1c-spin | 62.3 | **9.5** | 10.1 | -84.7 % / -83.8 % | 60.9 / 8.9 / 9.0 | 170.6 / 100.6 / 3.3 |
|  | 1c-park | 61.8 | **9.6** | 10.2 | -84.5 % / -83.5 % | 60.1 / 8.7 / 9.1 | 170.3 / 106.9 / 7.0 |
|  | 2c-spin | 51.7 | **9.6** | 10.2 | -81.3 % / -80.3 % | 49.8 / 9.6 / 9.6 | 160.4 / 281.6 / 28.5 |
|  | 2c-park | 50.9 | **9.9** | 10.2 | -80.6 % / -80.0 % | 49.5 / 9.5 / 10.0 | 170.2 / 309.5 / 42.1 |
| `big` (round trip) | 1c-spin | 33.9 | **21.4** | 22.3 | -36.8 % / -34.2 % | 33.2 / 21.2 / 21.7 | 290.7 / 133.6 / 7.2 |
|  | 1c-park | 33.2 | **22.7** | 22.1 | -31.5 % / -33.4 % | 32.2 / 22.3 / 21.3 | 294.4 / 143.9 / 13.8 |
|  | 2c-spin | 28.8 | **21.7** | 22.4 | -24.7 % / -22.1 % | 27.3 / 21.1 / 21.6 | 248.5 / 227.1 / 25.4 |
|  | 2c-park | 29.9 | **21.9** | 22.6 | -26.8 % / -24.4 % | 29.4 / 21.5 / 21.8 | 250.5 / 280.0 / 56.9 |
| `fib` (actor) | 1c-spin | 3,388.3 | **133.3** | 138.3 | -96.1 % / -95.9 % | 3,226.8 / 126.5 / 131.3 | 1,200.2 / 2,697.3 / 30.0 |
|  | 1c-park | 3,420.4 | **135.3** | 132.1 | -96.0 % / -96.1 % | 3,185.9 / 126.3 / 129.2 | 1,223.6 / 2,760.6 / 53.2 |
|  | 2c-spin | 2,598.8 | **90.4** | 90.3 | -96.5 % / -96.5 % | 2,475.9 / 86.5 / 88.3 | 679.6 / 5,248.6 / 59.3 |
|  | 2c-park | 2,413.5 | **89.8** | 95.0 | -96.3 % / -96.1 % | 2,268.5 / 86.4 / 83.0 | 670.7 / 5,519.0 / 58.5 |
| `chameneos` (meeting) | 1c-spin | 59.4 | **35.3** | 35.3 | -40.5 % / -40.6 % | 58.4 / 33.9 / 34.2 | 613.3 / 269.1 / 14.7 |
|  | 1c-park | 65.4 | **33.3** | 33.9 | -49.0 % / -48.1 % | 64.0 / 32.7 / 32.9 | 617.0 / 296.5 / 27.8 |
|  | 2c-spin | 72.2 | **54.3** | 55.7 | -24.7 % / -22.8 % | 70.7 / 53.6 / 52.7 | 630.7 / 767.6 / 144.2 |
|  | 2c-park | 78.5 | **53.8** | 56.6 | -31.5 % / -28.0 % | 75.2 / 52.1 / 54.4 | 638.3 / 962.5 / 365.4 |
| `bank-transaction` (transfer) | 1c-spin | 283.6 | **143.5** | 142.4 | -49.4 % / -49.8 % | 263.1 / 135.8 / 134.3 | 830.4 / 390.8 / 22.1 |
|  | 1c-park | 278.7 | **145.7** | 148.8 | -47.7 % / -46.6 % | 262.2 / 136.7 / 135.2 | 859.9 / 409.4 / 51.9 |
|  | 2c-spin | 179.3 | **92.8** | 93.7 | -48.2 % / -47.7 % | 165.5 / 90.4 / 91.5 | 732.7 / 510.0 / 126.0 |
|  | 2c-park | 177.5 | **96.4** | 97.8 | -45.7 % / -44.9 % | 171.3 / 93.1 / 90.8 | 732.2 / 553.3 / 92.5 |

## Reading

**All 32 cells move the same way, and none is inside the spread.** The smallest gain is
−19 % (ping-pong 2c-spin, pass 2 — the cross-core cells are where pass 2 sits above pass 1,
below); the one-core cells of the five static shapes are −29 to −85 %, the two collapsed
2c-park cells are −99.2 % both (29.03 µs → 227 / 247 ns per round trip, 14.54 µs → 121 / 129
ns per hop — the §5 defect, gone, and gone under a hypervisor whose futex wake alone is 12
µs), fib is −96 % (194.2 → 7.64 ms at one core, 148.9 → 5.18 at two: 25 × / 29 ×, the nine
`LOG_INFO` lines per actor lifetime demoted and the O(n²) table growth fixed), chameneos is
−41 / −49 % at one core, and bank-transaction is −48 % at one core and −48 % at two (14.18 /
13.94 ms → 7.18 / 7.29, 8.96 / 8.88 → 4.64 / 4.82) — the same 2 : 1 as the ask-slot-table
session that produced its last commit (`../qb-branch-perf-ask-slot-table/`, 145.0 / 144.9 /
93.7 / 92.2 there for `82ea03a8` against 143.5 / 145.7 / 92.8 / 96.4 here: two sessions, two
builds, one level).

**Against the field, the candidate is the fastest framework in every one of the 32 cells**,
by 1.17 × (thread-ring 2c-park against CAF's 141.4) to 17.2 × (fork-join 2c-park against CAF's
170.2), and it sits **below the raw-thread floor in twelve of the sixteen two-core cells**:
counting 11.0 vs 23.7 / 59.8, thread-ring 2c-spin 112.3 vs 114.4 (pass 1; pass 2's 127.6 is
1.12 × — below), fork-join 9.6 / 9.9 vs 28.5 / 42.1, big 21.7 / 21.9 vs 25.4 / 56.9, chameneos
54.3 / 53.8 vs 144.2 / 365.4, ping-pong 2c-park 227.0 vs 25 467, thread-ring 2c-park 120.7 vs
13 008, bank-transaction 2c-spin 92.8 vs 126.0 — the floor pays one remote cache-line crossing
per message on its SPSC ring where qb's staging pipe moves a batch per flush (§9.7), and the
floor's condition variable pays the hypervisor's 12 µs wake where qb's park never has to wake
for a message. The four two-core cells above the floor are the ones that cross a core per
message with nothing to batch: ping-pong 2c-spin 1.06 × (222.0 vs 210.1 — level), the two fib
cells 1.52–1.54 × (creation, not messaging) and bank-transaction 2c-park 1.04 × (96.4 vs
92.5, level).

**What the grid leaves, named so the next axis is chosen from a figure and not a feeling:**

- **The one-core round trip is 66 ns, and a round trip is two dispatches.** counting measures
  the same dispatch at 8.7 ns when a burst of a million is staged and drained in batches;
  ping-pong's two actors alternate one event at a time, so every event pays a full
  `__flush_all__` → `consume_all` → route cycle of its own — 33 ns per hop against 9 batched.
  41 × the floor (1.6 ns, a `std::atomic` hand-off) and 2.2 × SObjectizer's 145.1, the best of
  the field here. That cycle — the per-pass cost of a core with one event in flight — is the
  next thing to profile, and this is the host with `perf`.
- **Pass 2 sits above pass 1 on the cross-core cells, by a level shift, not a spread.**
  thread-ring 2c-spin sorts 107.2 … 122.6 in pass 1 and 125.0 … 129.4 in pass 2 — the two
  launches do not overlap — and 2c-park 115.1 … 123.3 against 125.7 … 131.7; ping-pong 2c-spin
  207.2 … 245.6 against 224.4 … 257.9, 2c-park 213.5 … 252.1 against 232.8 … 255.4. Shipped
  sorts 159.4 … 187.1 and 273.1 … 320.5 on the two spin cells, so either pass is the same
  verdict; but the shift is the 9-minute control leg between the passes (8 min 40 s of it
  writing 61 MB of log per repetition through the guest's page cache) rather than the build,
  and it is the reason §9.11's interleaved launch census — not a grid — is the instrument
  for a cross-core cell: the 2026-09-06 census put thread-ring 2c-spin at 133.3 vs 138.8
  for two builds that a grid had ranked the other way. The one-core and the batched two-core
  cells (counting, fork-join, big, bank) do not shift — 0.0 to 0.6 ns between passes.
- **ping-pong 2c-park is 227 ns where the floor is 25.47 µs**, and that is the §5 collapse
  read from the other side: the floor is a condition variable under a hypervisor, qb parked is
  a core that answers before it ever sleeps (axis N: the park is inside `ev_run`, and a
  producer's `ev_async_send` is what ends it). It is not free — 2c-park is 5 ns above 2c-spin
  here (227.0 vs 222.0) and 8 above on thread-ring — but it is the only framework in the
  field whose park mode costs nanoseconds rather than the wake.
- **fib at one core is 133 ns per actor, 4.4 × the floor's 30** (the floor's node is one heap
  `Node` in a per-worker slot table and two ring messages, nothing else) — what an actor's
  lifetime costs once nothing is logged and the tables are dense: `addActor`, a registry
  slot, the two subscriptions, `kill`, and the pipe traffic of its two events. §11.3 lists
  what is left in it, and 9.0 × CAF's 1 200 is the margin it leaves. It is also the cell
  where g++ and MSVC differ most (133 against 199): the same source, one generation of the
  same tables.
- **bank-transaction at one core is 143.5 ns per transfer**, the ask round trip plus the
  transfer itself, 6.5 × the floor's 22.1 and 2.7 × SObjectizer's 390.8; at two cores 92.8,
  below the floor's 126.0. The slot table (QB-178) took the ask path to zero allocations;
  what remains is the two dispatches of every ask and the reply's route back, the same
  per-pass cost as the first residual.

At `f8eba11d` all nine of qb's own GitHub lanes are green (`cmake`, `sanitize`,
`sanitize-thread`, `coverage`, `abi-fingerprint`, `install-consume`, `doc-lint`,
`format-check`, `scaffold`); the g++-14 suite at the chain's last core commit `82ea03a8` is
in `../qb-branch-perf-ask-slot-table/README.md` (Release / ASan+UBSan / TSan 192 / 192 / 0
each, standalone SSL-off qb). `docs/TUNING.md` §13 carries the two-host
reading and the deltas against the Windows half.
