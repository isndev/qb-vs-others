// Shared qb setup for every benchmark in this adapter.
//
// It exists so that the two configuration levers -- worker placement and spin/park -- are written
// ONCE. Twenty-five Savina benchmarks each spelling their own engine setup is twenty-five chances
// for one of them to quietly get a different deal from the others.

#ifndef QVO_QB_SUPPORT_H
#define QVO_QB_SUPPORT_H

#include <qvo/harness.h>

#include <chrono>
#include <cstdlib>

#include <qb/main.h>
#include <qb/system/cpu.h>

namespace qvoqb {

// The park interval used when `wait=0`. Overridable so docs/TUNING.md can sweep it exactly as it
// sweeps CAF's work-stealing knobs; the default is that sweep's winner. Symmetry is the point --
// tuning a competitor's park setting while leaving your own arbitrary rigs the axis just as
// effectively as not tuning theirs at all.
inline std::chrono::microseconds park_interval() {
    if (const char *v = std::getenv("QVO_QB_PARK_US"))
        return std::chrono::microseconds{std::strtoull(v, nullptr, 10)};
    return std::chrono::microseconds{1000};
}

// Configures VirtualCore `index` and returns it ready for actors.
//
// PLACEMENT IS THE POINT OF qb. A VirtualCore is a pinned worker thread that owns its actors;
// measuring qb with that mechanism switched off measures a qb nobody would ship. The harness
// pins the PROCESS to a CPU set and hands it over in `qvo::pinned_cpus()`; each VirtualCore takes
// one CPU from that list, so qb gets its architecture while staying inside exactly the same CPU
// budget every other framework is given. CAF receives the identical treatment through a
// thread_hook and SObjectizer through a work-thread factory -- see their support headers.
//
// When the harness ran unpinned (--no-pin) the list is empty and the core is left with
// `qb::NoAffinity`, qb's own named opt-out, rather than an invented CPU index.
inline void configure_core(qb::Main &engine, int index, bool spin) {
    auto &core = engine.core(static_cast<qb::CoreId>(index));

    core.setLatency(spin ? qb::duration::zero() : qb::duration{park_interval()});

    const auto &cpus = qvo::pinned_cpus();
    if (cpus.empty()) {
        core.setAffinity(qb::CoreIdSet{qb::NoAffinity});
    } else {
        const int cpu = cpus[static_cast<std::size_t>(index) % cpus.size()];
        core.setAffinity(qb::CoreIdSet{static_cast<qb::CoreId>(cpu)});
    }
}

// The caveats every qb benchmark carries. Written once so they cannot drift between benchmarks.
inline std::vector<std::string> caveats() {
    std::vector<std::string> c{
        "qb's VirtualCores are pinned one per CPU from the harness's set -- the mechanism qb is "
        "built on. CAF and SObjectizer are given the same treatment through their own APIs "
        "(caf::thread_hook, so_5 work-thread factory), so the CPU budget is identical and no "
        "framework is measured with its placement mechanism switched off",
        "wait=1 maps to setLatency(0) (busy-spin, 100% CPU per core); wait=0 maps to a park cap "
        "on a condition variable that is signalled on every enqueue",
        "qb is built with QB_WITH_LOGGING at its shipped default (ON). Its logger writes at "
        "startup, outside the measured window, but its thread shares the pinned CPU set. This is "
        "left ON deliberately: turning it off would improve qb's figure and no other framework "
        "gets an equivalent subtraction"};
    if (!qb::CPU::ThreadPinningSupported())
        c.emplace_back("THIS PLATFORM HAS NO REAL THREAD PINNING -- qb::CPU::ThreadPinningSupported() "
                       "is false, so the placement above did not happen and the figure is not "
                       "comparable with a pinned host");
    return c;
}

}  // namespace qvoqb

#endif  // QVO_QB_SUPPORT_H
