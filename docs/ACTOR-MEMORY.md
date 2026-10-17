# Live actor memory probe

`qvoprobe-actor-memory-{qb,caf,sobjectizer,baseline}` reports process memory while a verified population stays alive. It is a memory instrument, outside the `qvo-*` timing roster. `baseline` is a raw-node floor, not a framework contestant. No timing or framework-to-floor ratio is emitted.

## Contract

One executable invocation is one fresh process and one population. Pass `--actors N --cores C`; N is 0..60000, C is 1..16. Keep C fixed across N and frameworks. The N bound respects the existing qb actor-slot capacity, including one infrastructure actor per core; it does not alter ActorId. All adapters use a park profile and report `pinned:false`. The `cores` field is the configured worker budget, not an observed OS-thread count or fixed physical CPU placement. `infrastructure_ready` counts dispatched infrastructure sentinels (floor: readiness messages); CAF/SObjectizer sentinels do not prove that each OS worker executed a callback. Sweep environment overrides are refused. Auxiliary framework threads and the common deadline thread remain part of the process.

The common ledger is allocated and touched before sampling. Every target identity must be born exactly once and execute its readiness handler exactly once. A constructor alone is insufficient. Each target must remain alive through the memory read; destruction, missing identities and duplicate readiness refuse the document. A common 100 ms settling interval follows readiness, but the exact identity ledger, not that delay, proves life. One infrastructure actor/agent per configured worker budget is excluded from N and also present in the N=0 control. The floor uses one readiness message per worker instead.

Three phases are retained:

- `before`: ledger, deadline and observer tables exist; the actor runtime constructed by `adapter.run` has not started. Process static initializers, CAF `init_global_meta_objects` and an already initialized qb logger are included in this baseline. The deadline constructor waits for its thread to signal readiness before returning, so the watchdog has entered its body before this capture.
- `live`: runtime active and every target verified alive. A second check after reading refuses an early disappearance.
- `destroyed`: every target destruction verified, all runtime owners and worker threads destroyed; the common proof/observer storage still exists as at `before`.

CAF actor state destruction marks logical actor termination; the adapter also clears all actor handles and destroys the actor system before the last memory sample. SObjectizer agents share one cooperation because their lifetimes end together; the individual-FIFO pool is the existing many-agent binder. qb uses its default actor subscriptions plus one readiness event. A partial floor `Mesh::start` failure stops and joins the workers already created before propagating the startup error; the normal path calls `run` only once. The floor uses heap nodes and the existing `Mesh` queues; it allocates nodes on the controller thread and has no framework registry or per-actor mailbox. These are explicit differences, not byte-for-byte equivalent runtimes.

## Memory fields and interpretation

`rss_kib` is current process resident memory. `secondary_kib` is labelled by `secondary_metric`: Windows `private_commit`; Linux and macOS `rss_high_water`. The units are KiB, 1024 bytes. macOS reads `resident_size` and `resident_size_max` from `MACH_TASK_BASIC_INFO` in the shared memory reader. Invalid or incomplete reads have no numeric replacement; zero is accepted only when the source actually reports zero.

Always retain N=0 runs at the same C alongside several positive N values and repeat each configuration in fresh processes, interleaving framework/population order. Record binary hashes, exact source/dependency revisions, compile commands, allocator, host/OS and the command with each raw stdout/stderr/exit receipt. No published memory comparison follows from the correctness controls below. Use a quiet reserved host; do not overlap builds, tests, services or other measurement campaigns.

`live - before` includes runtime startup, target population, subscription/registry/queue storage, transient allocation retained by the allocator and thread stacks. It is **not** an actor size, even after division by N. N=0 controls and several populations at fixed C expose fixed costs and slope without making allocator history disappear. Retain negative deltas or overlap as observations/uncertainty; do not clamp them into a positive actor cost. `destroyed` does not have to return to `before`: process allocators may retain released pages. High-water memory cannot be interpreted as currently live or returned memory.

The qb breakdown is separate from RSS:

- `object_size_bytes` is C++ `sizeof` of the concrete probe actor, including its ledger reference and index. It excludes dynamic allocation extent. The corresponding type-size caveats apply to other adapters too.
- `qb_thread_arenas` contains one owner-thread snapshot per qb core, taken by its Sentinel when that Sentinel dispatches its readiness `Check`: `snapshot_stage=sentinel_readiness_after_startup_init`, `scope=whole_owner_thread_pooled_arena`. Each record has `live_blocks`, `chunks` and `chunk_bytes`. The observer array is sized and touched in `prepare`, before `before`; each Sentinel writes only its own slot. The array is read only after the actor runtime has joined, so the controller never reads another thread's TLS. No actor fields or readiness protocol are added.
- The snapshot is possible at Sentinel readiness because `Main::start_thread` constructs every local factory, runs synchronous startup initialization and passes the startup barrier before the workflow dispatches that `Check`. Both actor types in this probe have synchronous `onInit`. This is an earlier, explicitly named stage than the RSS `live` capture; the values are not a simultaneous RSS/arena snapshot. No arbitrary activation or asynchronous-init workload is covered by this ordering argument.
- `infrastructure_actors` separately records the one Sentinel per core. The Sentinel is statically checked to fit the pooled arena. It makes each snapshot nonempty, but `live_blocks` is the **whole thread's** pooled arena count and is not asserted equal to N plus infrastructure. Allocations above `thread_arena::max_small` (currently 1024 bytes), or over its granule alignment (currently 16), bypass these pooled counters. `chunk_bytes` is retained chunk capacity, not live object bytes or RSS. Never sum these values with global region/slab diagnostics: the layers can overlap.
- `qb_global_regions` reports existing allocator diagnostics (`cached_bytes`, `lent_bytes`, `peak_bytes`) under the allocator's existing lock. They cover the process's regions, not just this population; no actor/arène attribution is claimed.
- `subscription_storage_bytes` and `oninit_frame_bytes` are `null`. Custom subscriptions live in retained dense tables whose exact per-population storage is not exposed; default subscriptions are embedded dispatch pointers already included in the actor object. Counting one readiness subscription does not establish its allocated bytes. There is also no frame-size accounting API used here. This qb `onInit` completes without suspension; this instrument does not measure suspended initialization frames or invent a zero frame size.

A refused probe exits 1 and emits `verified:false` with empty samples. Invalid CLI exits 2 before creating the runtime. A global deadline bounds shutdown as well as readiness; a deadline exit has stderr and no accepted document. Preserve both streams and the exit code. Do not pass this schema to timing report tools.

## Targeted validation

Build only the configured frameworks' memory probes and corresponding `qvoctl-actor-memory-missing-<framework>` targets, plus `qvoctl-process-memory` and `qvoctl-actor-memory-ledger`. With a multiconfiguration generator, select the same configuration for all targets. Existing options `QVO_BUILD_PROBES` and `QVO_BUILD_CONTROLS` own these targets. Disabled frameworks are not silently validated; name the selected subset explicitly in the command below.

```sh
python3 tools/actor-memory-control.py --self-test
# Run both small C++ controls from the selected configuration's bin directory.
<bin>/qvoctl-process-memory
<bin>/qvoctl-actor-memory-ledger
python3 tools/actor-memory-control.py --bin <bin> --out <new-control-directory>
# Exercise both owner slots with a new output directory:
python3 tools/actor-memory-control.py --bin <bin> --frameworks qb --cores 2 --out <new-two-core-control-directory>
```

On Windows use `python` when that is the installed interpreter; the control invokes executable paths through argv arrays and uses the platform executable suffix. The default framework list requires all four binaries. `--frameworks qb baseline` is an explicit partial validation, not proof of CAF or SObjectizer.

The synthetic JSON controls accept complete populations, the floor role and real zero readings, and reject missing/readiness/destruction/identity/metric/value defects with specific diagnostics. The pure C++ ledger subject additionally proves duplicate births/readiness and invalid identity refusal. Each real adapter has a separate control executable that omits the last target while declaring the original N; the process-boundary control requires its exact plant-fired marker once, exit 1, the population diagnostic and empty samples. A normal probe has no command-line plant switch. Small successful populations N=0 and N=3 must preserve exact counters. Group Release, ASan/UBSan, TSan and coverage validation as applicable; native Windows remains separate from Mac proofs.

## Existing pipe probe output

New `qvoprobe-pipe-footprint` console output uses `secondary_kb` and `delta_secondary_kb` with `secondary_metric`, instead of labelling Linux high-water RSS as `private_kb`. Historical raw files keep their original bytes and labels. This correction does not turn that core/pipe-footprint workload into an actor population probe.

The qb adapter uses the current source tree’s existing `region_stats` diagnostics. An older shipped tree lacking that API must disable these probes; it is not silently given invented region counters.

The qb adapter also refuses a run when `engine.hasError()` is true after joining; population readiness alone cannot override an engine failure. Its exception path still stops and joins before rethrowing.

The synthetic arena positives deliberately permit more pooled blocks than target actors plus infrastructure. Controls reject a missing/duplicate core, a mismatched stage/scope, invalid counters and missing infrastructure. A missing Sentinel snapshot must also refuse the actual probe before any accepted memory JSON is emitted.
