# Wave2: five-cell paired follow-up

This is a **prepared procedure**, not a measurement. Run it on a quiet Linux host only after
the H3 benchmark has ended. The original [Wave2 run](https://github.com/isndev/qb-dev/actions/runs/37860027295)
used qb control `31ad945c6917`, qb candidate `4209e6809f91`, and the same QVO harness
`44e3e7439421` on both sides. Pin those runtime revisions again for a direct follow-up. The
new result reader in this QVO branch changes no benchmark or harness code.

The existing `tools/launch-census.py` already runs two binaries sequentially, alternates
AB/BA first position, saves every raw result, and rejects an unverified cell. The root's
`dev/bench/qvo-wave2.py monitor` samples host competition, isolates the runner on spare
physical CPUs, and restores its thread affinities even after a child failure. Do not call
either tool without the other for a publishable follow-up.

The table is a **sequential field observation**: a positive percentage means the candidate
was slower, and a negative percentage means it was faster.

| Shape | Config | Field candidate/control | Reason to remeasure |
|---|---|---:|---|
| fork-join | 2c-park | +15.49% | largest slowdown alert |
| fib | 1c-spin | +9.09% | clearest fib slowdown alert |
| counting | 1c-spin | −10.30% | repeatable gain across the field's four configs |
| bank-transaction | 2c-spin | −10.19% | largest bank gain |
| big | 1c-spin | −7.11% | independent burst gain |

Ping-pong 2c-park already has **24 accepted paired AB/BA launches** in the original artifact:
its +12.71% field alert became +0.67% paired median (IQR 2.30%). A new ping-pong launch is
optional as a same-session sentinel if the binaries or host state change; it is not part of
the five-cell minimum. Do not treat the sequential field deltas above as causal.

## Preflight and the five measurements

Use two checkouts named `control/` and `candidate/`, as in the Wave2 workflow, and keep the
identical QVO harness revision in both. Build with the same GCC 14.2.0, Release `-O3 -DNDEBUG`,
toolchain and CMake cache choices. Run the existing `negative-control.py` on both builds and
`candidate/dev/bench/qvo-wave2.py verify-build` on their cache, compile and link commands;
require `findings: []`. Its current link-command check names ping-pong and thread-ring only:
also inspect the five selected `ninja -t commands qvo-qb-savina-<shape>` final link lines
on both builds for matching flags and library inputs after checkout paths are normalized.
Record binary SHA-256 values before and after measuring. No build,
suite, VM or other benchmark may share the host with these five cells.

On io, `candidate/dev/bench/qvo-wave2.py self-test` exercises the quiet-monitor failure and
runner-affinity restoration controls without measuring a benchmark. The normal workflow's
`wait_quiet` function takes **two consecutive** successful
`candidate/dev/bench/http-ab.py quiet --cpus 8,9 --out <snapshot>` readings, up to 20 tries
30 seconds apart. Reuse it before the first cell and after each cell. Let the host settle
for 120 seconds after the builds. The following is the measurement step **per cell**;
substitute one row of the table and use a new attempt directory on every retry:

```sh
# Run only when H3 has released io and the preflight above is green.
set -euo pipefail
cpus=8,9
shape=fork-join
config=2c-park
attempt=1
results="$GITHUB_WORKSPACE/qvo-wave2-five-cell"
stage="$results/$shape-$config-attempt-$attempt"
mkdir -p "$stage"
control_bin="control/qb-vs-others/build/qb922/bin/qvo-qb-savina-$shape"
candidate_bin="candidate/qb-vs-others/build/qb922/bin/qvo-qb-savina-$shape"
test -x "$control_bin" && test -x "$candidate_bin"
sha256sum "$control_bin" "$candidate_bin" > "$stage/binaries-before.sha256"
python3 candidate/dev/bench/http-ab.py quiet --cpus "$cpus" --out "$stage/before-1.json"
sleep 30
python3 candidate/dev/bench/http-ab.py quiet --cpus "$cpus" --out "$stage/before-2.json"

python3 candidate/dev/bench/qvo-wave2.py monitor \
  --out "$stage/quiet" --cpus "$cpus" --isolate-runner -- \
  python3 candidate/qb-vs-others/tools/launch-census.py \
    --out "$stage/census" \
    --bin "control=$control_bin" --bin "candidate=$candidate_bin" \
    --config "$config" --launches 12 --alternate-order \
    --repetitions 3 --warmup 1 --cpus "$cpus" \
  > "$stage/monitor.log" 2>&1

python3 candidate/qb-vs-others/tools/analyze-paired-census.py \
  --results "$stage/census" --quiet-summary "$stage/quiet/summary.json" \
  --config "$config" --launches 12 --cpus "$cpus" \
  --binary-manifest "$stage/binaries-before.sha256" --require-binaries \
  > "$stage/analysis.json"
sha256sum "$control_bin" "$candidate_bin" > "$stage/binaries-after.sha256"
cmp "$stage/binaries-before.sha256" "$stage/binaries-after.sha256"
python3 candidate/dev/bench/http-ab.py quiet --cpus "$cpus" --out "$stage/after-1.json"
sleep 30
python3 candidate/dev/bench/http-ab.py quiet --cpus "$cpus" --out "$stage/after-2.json"
```

Run rows **one at a time**. If the monitor or reader exits nonzero, keep that attempt's
raw files as rejected evidence, wait for quiet again, and retry into `attempt-2`, never
into the same directory. The reader refuses a contaminated window, failed runner-affinity
restoration, wrong command, incomplete or unbalanced 12-pair order, failed checksums,
changed workload, unmatched compiler flags, a swapped or duplicated control/candidate
checkout path, a binary hash not matching the pre-run manifest, or extra/missing result
files. The monitor's command preserves each original checkout path when an artifact is
downloaded elsewhere. Offline re-analysis can omit `--require-binaries` when the executables
are absent; it then reports `binary_hashes_checked: false` and must rely on the archived
pre/post SHA-256 files for the byte comparison. A clean on-runner output
contains 12 control and 12 candidate launches, six of each first position, with three
measured repetitions and one warmup in each process. Save the pre/post quiet snapshots,
monitor summary, analysis JSON, build equivalence, binary hashes and both runtime SHAs
beside the accepted attempt.

## Read the result

The reader reports per-launch median ns/unit, the median and IQR of the **12 paired percent
deltas**, a deterministic 10,000-resample bootstrap interval for that median, and medians
for control-first and candidate-first launches separately. Its `screening_only` flag is a
triage hint, never a causal verdict: it requires an absolute paired median above 5.6% (the
largest paired IQR among the eight existing ping-pong/thread-ring census cells), the same
sign under both orders and a bootstrap interval excluding zero. At 3–6%, extend that cell
to 24 paired launches. Below 3%, report the observed distribution without attributing it.

Any surviving candidate effect still needs the FAIRNESS layout check: three byte-identical
copies of each binary in shuffled launch order, followed by a matched rebuild of **both**
sides with `-falign-functions=64 -falign-loops=64 -falign-jumps=64`. A difference that moves
with page placement or disappears under matched alignment is not credited to the runtime.
Do not publish a table until the quiet, correctness and layout controls all agree.
