# qb `QB-65` — one generation per signal (Windows 11, MSVC 19.51, i9-12900K)

Control `9dfe1e08` (one pending-signal slot, the stop token polled every pass) against candidate `72f1fb5f` (committed as `fa925cc8`: the same tree, its message amended to record these figures; one
generation per signal number plus one global generation; the stop token folded into it, so the pass reads ONE relaxed
load and compares it with a register). qb adapter only, `--cpus 0,2` (two P-cores), quiet host.

## Verdict

| cell / probe | multi-copy, random order | single copy, random order, 100 rounds |
|---|---|---|
| ping-pong 1c spin | **0.0 %**, −0.3 % | −0.3 % (A/A −0.1 %) |
| ping-pong 2c spin | **+3.0 %**, +2.5 % | +4.6 % (A/A −1.3 %) |
| thread-ring 2c spin | **+6.1 %**, +0.5 % | +2.0 % (A/A +0.9 %) |
| pass-cost k = 1 | −2.3 % … −2.6 % | |
| ask-cost | −6.8 % | |

The pass does less work (the probe) and the one-core cells are level. The two-core SPIN cells lose 2.5–3 % (ping-pong;
thread-ring is too noisy to pin, 0.5–6 %), and the loss is not the signal logic: putting back ONLY the control's
per-pass stop-token poll in front of the candidate's check (`s2/`, `cand-s2`, same pass cost as the control: 15.52 vs
15.53 ns) brings thread-ring 2c back on the A/A's figure and halves the ping-pong 2c gap. This is the idle-poll pacing
`VirtualCore.cpp` already documents (Huly QB-180): a spinning core's 2c exchange gets SLOWER when its idle pass gets
faster, and the candidate's pass is faster. The fix belongs to the pacing, not to the signal check (follow-up issue).

Ruled out along the way, each with its own census: VirtualCore's SIZE alone (`lottery/`: +8 / +64 / +92 / +256 bytes
move the 2c cells non-monotonically, −0.4 … +4.2 %), VirtualCore cache-line alignment (`align/`: `alignas(64)` changes
nothing for the candidate), the worker frame's alignment (`frame/`), and an `lfence` before the idle pass's
clock read (`lfence/`: costs the control as much as the candidate).

The `nops/` control adds 8 / 16 / 24 **executed instructions per pass** as well as moving
the instructions that follow. Its candidate ping-pong 2c results span +1.3 to +5.3 %,
but this combines extra work, a changed polling cadence and code placement. It cannot
rule out placement independently. `scripts/patch-nops.py` is retained as the historical
producer; its claim that nothing else changes is too strong. A pure placement control
would keep the pass's executed work unchanged, for example with padding on a cold branch,
and would need its own quiet-host comparison. No such new experiment is claimed here.

## Method — and the two traps it had to get past

* **Order.** The first scripts rotated a fixed cycle, so the A/A copy always ran right after the control: its A/A came
  out negative four runs out of four. From `random/` on, every round is a seeded Fisher–Yates order (`orders.txt`).
* **Physical placement.** Two byte-identical copies of one executable, in two files, differ by up to ±3–4 % on the 2c
  spin cells (each file has its own physical pages, so its hot code sits in different sets of the physically indexed
  caches). `copies/` and `lfence/` therefore run 4 (resp. 3) physical copies per side and compare the MEDIAN OF THE COPY
  MEDIANS; the spread inside the control group is the yardstick.

`summary.txt` / `probe.txt` at this level are the first, fixed-order A/B (its cells table printed empty there because a
PowerShell `$r` probe loop clobbered `$R`, the results directory; recomputed in `census/summary-recomputed.txt`).
Every experiment directory holds its `summary.txt` (and `orders.txt` from `random/` on); the per-run JSON files are in
`raw.tar.xz`. The scripts are in `scripts/`. The archived `nostop` variant had the same
case-insensitive `$R`/`$r` collision after eight probe rounds. Its producer now uses
`$resultDir` and `$probeRound`, checks the summarizer exit and requires a non-empty summary.
The retained `nostop/summary.txt` is recovery evidence; this repair does not claim it was
produced successfully by the uncorrected script. Its 720 archived result documents remain unchanged.
