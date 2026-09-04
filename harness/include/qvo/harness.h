// qvo — the neutral measurement harness shared by every framework adapter.
//
// This header is the ONLY timing, verification and reporting code in the repository. No framework
// adapter is allowed its own. That is mechanism 1.3 of FAIRNESS.md: whatever bias exists in how a
// run is timed applies identically to every framework, because it is the same object code.
//
// A benchmark implementation is a `main()` that hands `qvo::run()` a spec and a body:
//
//     int main(int argc, char **argv) {
//         return qvo::run(argc, argv, qvo::Spec{...}, [](const qvo::Params &p, qvo::Watch &w) {
//             ... build the actor system ...
//             w.start();                       // <- first workload message about to be injected
//             ... run to completion ...
//             w.stop();                        // <- terminal condition observed
//             return qvo::Answer{checksum};
//         });
//     }
//
// The body MUST return an Answer. The harness compares its checksum against the spec's expected
// value, which is computed WITHOUT any framework. A body that returns the wrong checksum produces
// no timing at all -- see FAIRNESS.md section 0.

#ifndef QVO_HARNESS_H
#define QVO_HARNESS_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace qvo {

using clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------------------------

// Benchmark parameters arrive as `--param name=value` and are always integers: every Savina
// benchmark is parameterised by counts. A benchmark declares its parameters with defaults in its
// Spec; asking for one that was not declared is a hard error, not a zero.
class Params {
public:
    void set(const std::string &name, long long value);
    long long get(const std::string &name) const;   // aborts if undeclared -- never silently 0
    bool has(const std::string &name) const;
    const std::map<std::string, long long> &all() const noexcept { return values_; }

private:
    std::map<std::string, long long> values_;
};

// ---------------------------------------------------------------------------------------------
// The measured window
// ---------------------------------------------------------------------------------------------

// Watch marks the workload window: from the instant the first workload message is injected into a
// running, warm actor system, to the instant the terminal condition is observed.
//
// Framework construction, actor creation and shutdown are deliberately OUTSIDE it -- they are
// measured separately as setup/teardown and reported alongside, so that a framework cannot look
// fast by having an expensive startup excluded and cheap messaging, nor slow for the reverse.
//
// start()/stop() are safe to call from any thread; the last call to each wins, which is what lets
// an actor running on a worker thread stop the watch at the moment it observes completion.
class Watch {
public:
    void start() noexcept;
    void stop() noexcept;

    bool started() const noexcept { return started_; }
    bool stopped() const noexcept { return stopped_; }
    double work_ns() const noexcept;

private:
    clock::time_point t0_{};
    clock::time_point t1_{};
    bool              started_{false};
    bool              stopped_{false};
};

// ---------------------------------------------------------------------------------------------
// The verified answer
// ---------------------------------------------------------------------------------------------

// Every benchmark reduces its work to one 64-bit checksum. The reduction must depend on EVERY
// message the benchmark's semantics require to be delivered, so that a dropped, coalesced or
// duplicated message changes it. `Spec::expected` computes the same value with no framework
// involved -- plain arithmetic, or a sequential reference implementation.
struct Answer {
    std::uint64_t checksum{0};

    // Optional: the number of messages the implementation believes it delivered. Reported, and
    // cross-checked against Spec::expected_messages when that is non-zero. This catches the case
    // where a framework reaches the right checksum by a different amount of work.
    std::uint64_t messages{0};
};

// ---------------------------------------------------------------------------------------------
// The specification
// ---------------------------------------------------------------------------------------------

struct Spec {
    std::string benchmark;        // e.g. "savina/thread-ring"
    std::string framework;        // e.g. "qb"
    std::string framework_version;// e.g. "3.1.0"

    // Declared parameters and their defaults. Savina's own default is used wherever this
    // repository has one; deviations are recorded in benchmarks/savina/<name>.md.
    std::map<std::string, long long> params;

    // The framework-free expected checksum. Called once, before the body.
    std::function<std::uint64_t(const Params &)> expected;

    // Optional: framework-free expected message count. 0 means "not asserted".
    std::function<std::uint64_t(const Params &)> expected_messages;

    // The unit the report divides a repetition's wall time by, so a figure stays comparable
    // across parameter values and across benchmarks: "round trip" for a ping-pong, "hop" for a
    // ring, "message" for a fan-out. `work_units` returns how many of them one repetition
    // performs. Both are written into the JSON and read back by tools/report.py; a benchmark
    // that declares neither is reported per message when expected_messages is set, else per
    // repetition. The pair is declared ONCE, in the benchmark's spec header, so every framework's
    // document carries the same denominator -- the report never guesses one from the name.
    std::string work_unit;
    std::function<std::uint64_t(const Params &)> work_units;

    // Machine-readable caveats -- anything that could not be held equal across frameworks for
    // this benchmark. Rendered in the report NEXT TO the number it affects (FAIRNESS.md 1.3).
    std::vector<std::string> caveats;

    // The framework document/example this implementation is modelled on (FAIRNESS.md 1.1).
    std::string idiom_source;
    std::string idiom_note;
};

using Body = std::function<Answer(const Params &, Watch &)>;

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------

// Parses the CLI, applies and VERIFIES CPU affinity, runs warmup + repetitions, verifies each
// answer, and emits one JSON document. Returns 0 only if every repetition verified.
//
// Exit codes -- tools/run.py reads them, so they are a contract:
//   0  every repetition verified
//   1  ran, but at least one repetition FAILED verification (a wrong checksum is a defect)
//   2  fatal: bad CLI, pin refused, no expected(), cannot write the output
//   3  NOT APPLICABLE: the body called qvo::not_applicable() -- see below
//
// CLI:
//   --repetitions N     measured repetitions (default 5)
//   --warmup N          unmeasured warmup repetitions (default 1)
//   --param k=v         set a declared parameter
//   --cpus 0,2,4,6      affinity set; the harness ABORTS if the pin is refused
//   --no-pin            run unpinned, and record `pinned:false` so the report can exclude it
//   --out FILE          JSON destination (default: stdout)
//   --describe          print the spec as JSON and exit without running
int run(int argc, char **argv, Spec spec, Body body);

// Declares that this framework cannot express the configuration it was asked to run, and says
// why. The harness writes a result document carrying the reason (`verified:false`,
// `not_applicable:"<reason>"`, no timings) and exits 3, which tools/run.py reports as `n/a`
// rather than as a failure and tools/report.py renders next to the row.
//
// This is a third verdict, deliberately distinct from both others. A cell that is quietly
// omitted reads as "not measured" and invites the reader to assume the best; a cell filled by
// measuring a DIFFERENT configuration under this label is worse. The known case: an adapter that
// runs every actor on a private thread parked on a condition variable (CAF's `detached`) has no
// spin mode to switch on, so its `wait=1` cell is neither a measurement nor a failure.
//
// Call it from the body, before the workload window, when the Params name a configuration the
// adapter has no honest counterpart for. It never returns.
[[noreturn]] void not_applicable(const std::string &reason);

// ---------------------------------------------------------------------------------------------
// Small helpers usable from inside an implementation
// ---------------------------------------------------------------------------------------------

// A deterministic, cheap mixer. Used to build checksums that depend on message identity and
// order-insensitive aggregation, so a checksum cannot be reached by a shortcut.
inline std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Busy work with a data dependency the optimizer cannot delete. Several Savina benchmarks specify
// a per-message computation; using the same function in every framework keeps that constant.
std::uint64_t spin_work(std::uint64_t seed, int iterations) noexcept;

// Prevents the optimizer from deleting a computation whose result is otherwise unused.
void sink(std::uint64_t value) noexcept;

// ---------------------------------------------------------------------------------------------
// Worker placement
// ---------------------------------------------------------------------------------------------

// The CPU set the harness pinned the PROCESS to, in the order the user gave it.
//
// An implementation is expected to place its Nth worker thread on `pinned_cpus()[N % size()]`.
// This exists because process-level pinning alone is NOT an equal starting line: a runtime whose
// whole design is one worker per core, pinned, is measured without the mechanism it is built on,
// while a work-stealing pool that never pins is measured exactly as it ships. Handing every
// framework the same CPU list and asking each to place its own workers is the symmetric setting.
//
// All three frameworks compared here can do it, through their own public APIs:
//   qb           CoreInitializer::setAffinity(qb::CoreIdSet{cpu})
//   CAF          caf::thread_hook::thread_started(thread_owner::scheduler)
//   SObjectizer  a custom so_5::disp::abstract_work_thread_factory_t
// Empty when the run was started with --no-pin.
const std::vector<int> &pinned_cpus() noexcept;

// Pins the CALLING thread to one CPU. Returns false when the platform refuses, and never reports
// success for a pin that did not happen -- the failure mode qb's own documentation records on
// Apple Silicon, and the one that would silently invalidate every number here.
bool pin_this_thread(int cpu) noexcept;

// True when this platform has real per-thread pinning at all.
bool thread_pinning_supported() noexcept;

}  // namespace qvo

#endif  // QVO_HARNESS_H
