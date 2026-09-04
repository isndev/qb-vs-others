// Shared CAF setup for every benchmark in this adapter.
//
// Written once so that all twenty-five Savina benchmarks give CAF the same deal, and so that the
// deal is reviewable in one place by somebody who knows CAF better than this repository's author.

#ifndef QVO_CAF_SUPPORT_H
#define QVO_CAF_SUPPORT_H

#include <qvo/harness.h>

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/thread_hook.hpp>
#include <caf/thread_owner.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace qvocaf {

// Pins CAF's scheduler workers, one per CPU from the harness's set.
//
// WHY THIS EXISTS: qb's whole design is a pinned worker per core, and giving qb that while
// leaving CAF's pool to float would measure two different experiments. CAF's own public
// `thread_hook` interface is called ON the freshly started thread (its header says so: "To access
// a reference to the started thread use std::this_thread"), which is exactly the hook needed.
//
// Only `thread_owner::scheduler` threads are pinned. CAF's clock, printer and detached-actor pool
// threads are deliberately left free -- pinning them onto the same small CPU set would have them
// fight the workers, which is a handicap, not a placement.
class PinSchedulerThreads final : public caf::thread_hook {
public:
    explicit PinSchedulerThreads(std::vector<int> cpus) : cpus_(std::move(cpus)) {}

    void init(caf::actor_system &) override {}

    void thread_started(caf::thread_owner owner) override {
        if (owner != caf::thread_owner::scheduler || cpus_.empty()) return;
        const auto n = next_.fetch_add(1, std::memory_order_relaxed);
        if (!qvo::pin_this_thread(cpus_[n % cpus_.size()]))
            failed_.store(true, std::memory_order_relaxed);
    }

    void thread_terminates() override {}

    bool any_pin_failed() const noexcept { return failed_.load(std::memory_order_relaxed); }

private:
    std::vector<int>          cpus_;
    std::atomic<std::size_t>  next_{0};
    std::atomic<bool>         failed_{false};
};

// Builds the configuration every CAF benchmark uses.
//
// `cores` becomes CAF's worker budget under the key `caf.scheduler.max-threads` -- read from the
// pinned CAF's own libcaf_core/caf/scheduler.cpp, not from memory. This matters more than it
// looks: CAF's default worker count comes from hardware_concurrency, which on Windows ignores the
// process affinity mask, so a mistyped key would leave CAF spawning 24 workers onto the few CPUs
// the harness pinned -- a self-inflicted handicap that would be published as a CAF result.
inline void configure(caf::actor_system_config &cfg, std::size_t cores, bool spin) {
    cfg.set("caf.scheduler.max-threads", cores);

    if (spin) {
        // CAF's workers poll aggressively, then moderately, then sleep. The profile below is the
        // winner of the sweep in docs/TUNING.md, NOT a value chosen by this repository's author:
        // the first profile tried here (poll=1e9, steal=1) made CAF almost 2x SLOWER than its own
        // defaults, which is what a competitor's author accidentally rigging a knob looks like.
        auto env_or = [](const char *name, std::size_t fallback) -> std::size_t {
            if (const char *v = std::getenv(name)) return std::strtoull(v, nullptr, 10);
            return fallback;
        };
        cfg.set("caf.work-stealing.aggressive-poll-attempts", env_or("QVO_CAF_AGGRESSIVE_POLL", 100));
        cfg.set("caf.work-stealing.aggressive-steal-interval", env_or("QVO_CAF_STEAL_INTERVAL", 10));
    }

    cfg.add_thread_hook<PinSchedulerThreads>(qvo::pinned_cpus());
}

// Asserts the worker budget actually landed. A settings dictionary accepts any key, so writing
// one proves nothing on its own; this reads it back under the exact name the scheduler reads.
inline void assert_budget(const caf::actor_system &sys, std::size_t cores) {
    const auto effective = caf::get_or(sys.config(), "caf.scheduler.max-threads", std::size_t{0});
    if (effective != cores) {
        std::fprintf(stderr,
                     "qvo: CAF worker budget did not take effect (asked %zu, config reports %zu). "
                     "Refusing to report a number measured under a thread budget that is not the "
                     "one every other framework was given.\n",
                     cores, effective);
        std::exit(2);
    }
}

inline std::vector<std::string> caveats() {
    return {
        "CAF's scheduler is a work-stealing pool. 'cores' is a thread BUDGET; the workers are "
        "pinned one per CPU via caf::thread_hook so the CPU set matches qb's exactly, but CAF "
        "still steals across them, which qb's actors cannot do. That is an architectural "
        "difference, and it cuts both ways: stealing costs on a two-actor ping-pong and pays on "
        "an unbalanced fan-out",
        "wait=1 raises caf.work-stealing.aggressive-poll-attempts (CAF's spelling of busy-spin) "
        "to the profile that measured fastest in docs/TUNING.md; wait=0 leaves CAF's shipped "
        "defaults, which on ping-pong are FASTER than any spin profile tried",
        "CAF 1.1.0 builds itself at C++17 -- its own CMake sets the standard -- while qb and the "
        "harness are C++20. Forcing CAF to C++20 was not done: it would measure a build CAF does "
        "not ship"};
}

}  // namespace qvocaf

#endif  // QVO_CAF_SUPPORT_H
