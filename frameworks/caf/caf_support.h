// Shared CAF setup for every benchmark in this adapter.
//
// Written once so that all twenty-five Savina benchmarks give CAF the same deal, and so that the
// deal is reviewable in one place by somebody who knows CAF better than this repository's author.

#ifndef QVO_CAF_SUPPORT_H
#define QVO_CAF_SUPPORT_H

#include <qvo/harness.h>

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/spawn_options.hpp>
#include <caf/thread_hook.hpp>
#include <caf/thread_owner.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace qvocaf {

// Pins CAF's scheduler workers, one per CPU from the harness's set.
//
// WHY THIS EXISTS: qb's whole design is a pinned worker per core, and giving qb that while
// leaving CAF's pool to float would measure two different experiments. CAF's own public
// `thread_hook` interface is called ON the freshly started thread (its header says so: "To access
// a reference to the started thread use std::this_thread"), which is exactly the hook needed.
//
// `thread_owner::scheduler` threads are always pinned. `thread_owner::pool` threads -- the one
// CAF gives each `caf::detached` actor -- are pinned only when the adapter runs its actors
// detached (the `caf-detached` variant below), and then on their OWN rotation over the CPU set,
// so the first detached actor lands on the first CPU regardless of how many workers were pinned
// before it. CAF's clock, printer and other system threads are deliberately left free --
// pinning them onto the same small CPU set would have them fight the workers, which is a
// handicap, not a placement.
//
// Both rotations run over the first `cores` CPUs of the harness set, not the whole set. The set
// is the same for every cell of the matrix (tools/run.py pins the process once), and `cores` is
// what the cell asks for: with cores=1 and a two-CPU set, two detached actors rotating over the
// SET would land on two cores and the "1 core" row would be a second copy of the 2-core one.
// Measured before this clamp existed: the two rows came back with the same bimodal distribution.
class PinSchedulerThreads final : public caf::thread_hook {
public:
    PinSchedulerThreads(std::vector<int> cpus, std::size_t cores, bool pin_pool)
        : cpus_(std::move(cpus)), pin_pool_(pin_pool) {
        if (cores >= 1 && cores < cpus_.size()) cpus_.resize(cores);
    }

    void init(caf::actor_system &) override {}

    void thread_started(caf::thread_owner owner) override {
        if (cpus_.empty()) return;
        std::atomic<std::size_t> *counter = nullptr;
        if (owner == caf::thread_owner::scheduler) counter = &next_worker_;
        else if (owner == caf::thread_owner::pool && pin_pool_) counter = &next_pool_;
        if (!counter) return;
        const auto n = counter->fetch_add(1, std::memory_order_relaxed);
        if (!qvo::pin_this_thread(cpus_[n % cpus_.size()]))
            failed_flag().store(true, std::memory_order_relaxed);
    }

    void thread_terminates() override {}

    // Process-wide rather than a member: CAF owns the hook once it is added and exposes it back
    // only to actor_system (`thread_hooks()` is private), so the adapter could never read a
    // member flag. One benchmark process runs one actor_system at a time, and configure()
    // clears it before each.
    static std::atomic<bool> &failed_flag() noexcept {
        static std::atomic<bool> flag{false};
        return flag;
    }

private:
    std::vector<int>          cpus_;
    bool                      pin_pool_;
    std::atomic<std::size_t>  next_worker_{0};
    std::atomic<std::size_t>  next_pool_{0};
};

// The two ways this adapter can run a CAF actor, selected per BINARY at compile time:
//
//   frameworks/caf/          -> `caf`          : every actor in the work-stealing pool, CAF's
//                                                 default and idiomatic placement;
//   frameworks/caf-detached/ -> `caf-detached` : every actor `caf::detached` -- one OS thread per
//                                                 actor, parked on a condition variable between
//                                                 messages (caf/detail/private_thread.cpp).
//
// WHY THE SECOND ONE EXISTS. In the pool, an actor made ready by a message sent FROM a worker is
// `delay()`ed -- prepended to the SENDER's own queue (scheduler.cpp, work_stealing::worker::delay)
// -- and the sender's worker runs it next unless another worker steals it first. On a two-actor
// ping-pong that means the receiver runs on the sender's thread every time, and CAF's "2 cores"
// figure is a one-core hand-off that never pays a cache-line transfer or a wake-up. That is
// CAF's design (locality), not a defect, and the `caf` row is the right idiomatic figure -- but it
// is NOT comparable to a framework whose two actors live on two pinned cores, and a reader
// deserves the cross-core number too. `caf::detached` is CAF's own, documented way to give an
// actor a thread of its own (spawn_options.hpp), which is the only public placement primitive CAF
// has; the price is that a detached actor can only park (there is no spin profile for a private
// thread), so the variant reports the spin cells as NOT APPLICABLE rather than inventing one.
#ifdef QVO_CAF_DETACHED
inline constexpr bool               kDetached     = true;
inline constexpr caf::spawn_options kSpawnOptions = caf::detached;
#else
inline constexpr bool               kDetached     = false;
inline constexpr caf::spawn_options kSpawnOptions = caf::no_spawn_options;
#endif

// Builds the configuration every CAF benchmark uses.
//
// `cores` becomes CAF's worker budget under the key `caf.scheduler.max-threads` -- read from the
// pinned CAF's own libcaf_core/caf/scheduler.cpp, not from memory. This matters more than it
// looks: CAF's default worker count comes from hardware_concurrency, which on Windows ignores the
// process affinity mask, so a mistyped key would leave CAF spawning 24 workers onto the few CPUs
// the harness pinned -- a self-inflicted handicap that would be published as a CAF result.
// The two work-stealing knobs a `wait=1` run may override from the environment, for the sweep in
// docs/TUNING.md section 1.1 and for nothing else: a document measured under an override carries
// the values in its caveats, so a sweep file can never be mistaken for a table cell.
inline std::optional<std::size_t> env_size(const char *name) {
    if (const char *v = std::getenv(name)) return std::strtoull(v, nullptr, 10);
    return std::nullopt;
}
inline std::optional<std::size_t> poll_override() { return env_size("QVO_CAF_AGGRESSIVE_POLL"); }
inline std::optional<std::size_t> steal_override() { return env_size("QVO_CAF_STEAL_INTERVAL"); }

inline void configure(caf::actor_system_config &cfg, std::size_t cores, bool spin) {
    cfg.set("caf.scheduler.max-threads", cores);

    if (spin) {
        // CAF's workers poll aggressively, then moderately, then sleep. The values written here
        // are CAF's OWN defaults (libcaf_core/caf/defaults.hpp: aggressive-poll-attempts=100,
        // aggressive-steal-interval=10), written explicitly so the sweep can override them and so
        // the document says what ran. They are the defaults because the sweep in docs/TUNING.md
        // section 1.1 found every more aggressive profile SLOWER: the first profile tried here
        // (poll=1e9, steal=1) made CAF almost 2x slower than itself, which is what a competitor's
        // author accidentally rigging a knob looks like, and no poll budget at any steal interval
        // beat 100/10. `wait=1` is therefore the same configuration as `wait=0`, and says so.
        cfg.set("caf.work-stealing.aggressive-poll-attempts", poll_override().value_or(100));
        cfg.set("caf.work-stealing.aggressive-steal-interval", steal_override().value_or(10));
    }

    PinSchedulerThreads::failed_flag().store(false, std::memory_order_relaxed);
    cfg.add_thread_hook<PinSchedulerThreads>(qvo::pinned_cpus(), cores, kDetached);
}

// A detached actor has no spin mode -- `private_thread::await()` is an unconditional
// `cv_.wait()` -- so `wait=1` is a configuration this variant cannot express. The harness has a
// verdict for exactly that; a number invented for the cell would be worse than no number.
inline void refuse_spin_if_detached(bool spin) {
    if (kDetached && spin)
        qvo::not_applicable("caf::detached actors park on a condition variable between messages "
                            "(caf/detail/private_thread.cpp); CAF has no spin mode for a private "
                            "thread, so wait=1 has no honest counterpart here -- read the caf row "
                            "for CAF's spin profile and this row for its cross-core park cost");
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

// Every pin the hook attempted must have taken. Threads start lazily (a detached actor's thread
// exists only once the actor is spawned), so this is asked at the END of a repetition, and a
// refused pin is a refused measurement -- the same rule the harness applies to the
// process-level pin. Without it the hook's failure flag was set and read by nobody.
inline void assert_pins_took() {
    if (PinSchedulerThreads::failed_flag().load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "qvo: CAF could not pin one of its threads to the requested CPU. "
                             "Refusing to report a number measured with a floating thread.\n");
        std::exit(2);
    }
}

inline std::vector<std::string> caveats() {
    if (kDetached)
        return {
            "every actor is spawned caf::detached: one OS thread per actor, pinned one per CPU "
            "from the harness's set through caf::thread_hook (thread_owner::pool), parked on a "
            "condition variable between messages. This is CAF's own placement primitive and the "
            "only way to make its ping-pong cross a core: in the work-stealing pool the receiver "
            "runs on the sender's worker (worker::delay prepends to the sender's queue), so the "
            "plain caf row never pays a cross-core hop. Read the two rows together",
            "a detached actor has no spin profile, so this variant reports wait=1 as not "
            "applicable rather than measuring a pool configuration under a detached label",
            "CAF 1.1.0 builds itself at C++17 -- its own CMake sets the standard -- while qb and the "
            "harness are C++20. Forcing CAF to C++20 was not done: it would measure a build CAF does "
            "not ship"};
    std::vector<std::string> c;
    if (poll_override() || steal_override())
        c.emplace_back("SWEEP DOCUMENT, NOT A TABLE CELL: caf.work-stealing.aggressive-poll-attempts=" +
                       std::to_string(poll_override().value_or(100)) +
                       " aggressive-steal-interval=" + std::to_string(steal_override().value_or(10)) +
                       " were overridden through QVO_CAF_AGGRESSIVE_POLL / QVO_CAF_STEAL_INTERVAL "
                       "(docs/TUNING.md section 1.1); CAF's shipped defaults are 100 / 10");
    c.insert(c.end(), {
        "CAF's scheduler is a work-stealing pool. 'cores' is a thread BUDGET; the workers are "
        "pinned one per CPU via caf::thread_hook so the CPU set matches qb's exactly, but CAF "
        "still steals across them, which qb's actors cannot do. That is an architectural "
        "difference, and it cuts both ways: stealing costs on a two-actor ping-pong and pays on "
        "an unbalanced fan-out",
        "wait=1 and wait=0 are the SAME configuration for CAF on this benchmark -- its shipped "
        "defaults (aggressive-poll-attempts=100, steal-interval=10). The sweep in docs/TUNING.md "
        "section 1 found every more aggressive profile SLOWER, because on a ping-pong the receiver "
        "runs on the sender's worker and polling harder only makes the idle worker steal it; CAF's "
        "fastest ping-pong is the one where nothing ever crosses a core. The cross-core figure is "
        "the caf-detached row",
        "in the work-stealing pool the receiver of a message sent from a worker is prepended to "
        "that worker's own queue (worker::delay), so CAF's two-core ping-pong runs both actors on "
        "one thread and never pays a cross-core hand-off; read this row as CAF's best-case "
        "locality, and caf-detached as its cross-core cost",
        "CAF 1.1.0 builds itself at C++17 -- its own CMake sets the standard -- while qb and the "
        "harness are C++20. Forcing CAF to C++20 was not done: it would measure a build CAF does "
        "not ship"});
    return c;
}

}  // namespace qvocaf

#endif  // QVO_CAF_SUPPORT_H
