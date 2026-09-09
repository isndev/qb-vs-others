// Shared SObjectizer setup for every benchmark in this adapter.
//
// Written once so all twenty-five Savina benchmarks give SObjectizer the same deal, and so a
// SObjectizer maintainer can review that deal in one file.

#ifndef QVO_SO_SUPPORT_H
#define QVO_SO_SUPPORT_H

#include <qvo/harness.h>

#include <so_5/all.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace qvoso {

// A worker thread that pins itself before running SObjectizer's body.
//
// WHY: qb's VirtualCores are pinned and CAF's workers are pinned through its thread_hook. Leaving
// SObjectizer's dispatcher threads floating would be the one framework measured under different
// placement, which is the asymmetry this whole repository exists to avoid. SObjectizer's public
// `abstract_work_thread_factory_t` (5.7.3+) is the supported way to supply them.
class PinnedWorkThread final : public so_5::disp::abstract_work_thread_t {
public:
    explicit PinnedWorkThread(int cpu) noexcept : cpu_(cpu) {}

    void start(body_func_t body) override {
        thread_ = std::thread([body = std::move(body), cpu = cpu_]() mutable {
            if (cpu >= 0) qvo::pin_this_thread(cpu);
            body();
        });
    }

    void join() override {
        if (thread_.joinable()) thread_.join();
    }

private:
    std::thread thread_;
    int         cpu_;
};

class PinningThreadFactory final : public so_5::disp::abstract_work_thread_factory_t {
public:
    explicit PinningThreadFactory(std::vector<int> cpus) : cpus_(std::move(cpus)) {}

    so_5::disp::abstract_work_thread_t &acquire(so_5::environment_t &) override {
        const int cpu =
            cpus_.empty() ? -1
                          : cpus_[next_.fetch_add(1, std::memory_order_relaxed) % cpus_.size()];
        auto                        t = std::make_unique<PinnedWorkThread>(cpu);
        auto                       *raw = t.get();
        std::lock_guard<std::mutex> lk(mutex_);
        owned_.push_back(std::move(t));
        return *raw;
    }

    // SObjectizer guarantees every acquire() is paired with exactly one release(), and that the
    // reference stays valid until then. The threads are kept owned here and destroyed with the
    // factory, which is the simplest scheme its documentation explicitly allows.
    void release(so_5::disp::abstract_work_thread_t &) noexcept override {}

private:
    std::vector<int>                                  cpus_;
    std::atomic<std::size_t>                          next_{0};
    std::mutex                                        mutex_;
    std::vector<std::unique_ptr<PinnedWorkThread>>    owned_;
};

// The MPSC queue lock, SObjectizer's half of the spin/park axis.
//
// `combined_lock_factory(d)` spins for `d` before falling back to a mutex; `simple_lock_factory()`
// is mutex + condition variable only. Read from dev/so_5/disp/mpsc_queue_traits/pub.hpp of the
// pinned SObjectizer. A spin budget far longer than any single hop is this framework's way of
// spelling busy-spin, which is what makes qb's setLatency(0) a comparable setting.
//
// The one knob a `wait=1` run may override from the environment, for the sweep in docs/TUNING.md
// section 1.2 and for nothing else: QVO_SO_SPIN_WAIT_US is the combined lock's waiting time in
// microseconds (its "spin" is a yield loop that re-reads the clock, dev/so_5/disp/mpsc_queue_traits/
// pub.cpp `combined_lock_t::wait_for_notify`; SObjectizer's own default is 1 ms), and 0 asks for
// simple_lock_factory under wait=1 -- the sweep's lower bound. A document measured under the
// override carries the value in its caveats, so a sweep file can never be mistaken for a table
// cell. Unset, the adapter's profile stands: 10 s.
inline std::optional<long long> spin_wait_override() {
    if (const char *v = std::getenv("QVO_SO_SPIN_WAIT_US")) return std::strtoll(v, nullptr, 10);
    return std::nullopt;
}

inline std::chrono::high_resolution_clock::duration spin_wait_budget() {
    if (const auto us = spin_wait_override(); us && *us > 0) return std::chrono::microseconds{*us};
    return std::chrono::seconds{10};
}

template <typename QueueParams>
void tune_queue(QueueParams &q, bool spin) {
    if (spin && !(spin_wait_override() && *spin_wait_override() == 0))
        q.lock_factory(so_5::disp::mpsc_queue_traits::combined_lock_factory(spin_wait_budget()));
    else
        q.lock_factory(so_5::disp::mpsc_queue_traits::simple_lock_factory());
}

// One binder for the whole coop.
//
//   cores >= 2  active_obj  -- one work thread per agent, SObjectizer's nearest equivalent to qb
//                             placing each actor on its own VirtualCore
//   cores == 1  one_thread  -- every agent on a single work thread
inline so_5::disp_binder_shptr_t make_binder(so_5::environment_t &env, int cores, bool spin) {
    auto factory = std::make_shared<PinningThreadFactory>(qvo::pinned_cpus());

    if (cores >= 2) {
        so_5::disp::active_obj::disp_params_t params;
        params.tune_queue_params([spin](auto &q) { tune_queue(q, spin); });
        params.work_thread_factory(factory);
        return so_5::disp::active_obj::make_dispatcher(env, "qvo-ao", std::move(params)).binder();
    }

    so_5::disp::one_thread::disp_params_t params;
    params.tune_queue_params([spin](auto &q) { tune_queue(q, spin); });
    params.work_thread_factory(factory);
    return so_5::disp::one_thread::make_dispatcher(env, "qvo-ot", std::move(params)).binder();
}

// The many-agent binder: a thread_pool of exactly `cores` work threads.
//
// active_obj gives every agent a thread, which is the right mirror of qb's "one actor, one
// VirtualCore" on a two-actor ping-pong and the wrong one at 60-120 agents, where it would run
// 60-120 threads against a 2-CPU budget and measure the OS scheduler. SObjectizer's shipped
// answer for that shape is thread_pool (dev/so_5/disp/thread_pool/pub.hpp): `thread_count(cores)`
// pinned threads and, with `fifo_t::individual`, one demand queue PER AGENT so agents of the
// same coop run concurrently on different threads -- the cooperation FIFO default would serialise
// the whole coop onto one thread and turn the 2-core cell into the 1-core one.
// `max_demands_at_once` is left at SObjectizer's shipped default (4): it is the batching knob
// CAF spells max-throughput=300 and qb spells "drain the pipe", and it is not tuned here.
// The pool's queue is MPMC, so its lock factory is the mpmc namespace's, not the mpsc one
// tune_queue() uses -- same two policies, same 10 s spin budget.
template <typename QueueParams>
void tune_pool_queue(QueueParams &q, bool spin) {
    if (spin && !(spin_wait_override() && *spin_wait_override() == 0))
        q.lock_factory(so_5::disp::mpmc_queue_traits::combined_lock_factory(spin_wait_budget()));
    else
        q.lock_factory(so_5::disp::mpmc_queue_traits::simple_lock_factory());
}

// The caveat a document measured under QVO_SO_SPIN_WAIT_US carries first, before the profile's
// own lines -- the same sentence CAF's sweep documents carry, so tools/check-report.py reads the
// two the same way.
inline void push_sweep_caveat(std::vector<std::string> &c) {
    if (const auto us = spin_wait_override()) {
        c.emplace_back("SWEEP DOCUMENT, NOT A TABLE CELL: the combined_lock waiting time under wait=1 was "
                       "overridden through QVO_SO_SPIN_WAIT_US=" + std::to_string(*us) +
                       (*us == 0 ? " (simple_lock_factory: mutex + condition variable, no spin)"
                                 : " microseconds") +
                       " (docs/TUNING.md section 1.2); the adapter's profile is 10 s, SObjectizer's own "
                       "default 1 ms");
    }
}

inline so_5::disp_binder_shptr_t make_pool_binder(so_5::environment_t &env, int cores,
                                                  bool spin) {
    auto factory = std::make_shared<PinningThreadFactory>(qvo::pinned_cpus());

    if (cores >= 2) {
        so_5::disp::thread_pool::disp_params_t params;
        params.thread_count(static_cast<std::size_t>(cores));
        params.tune_queue_params([spin](auto &q) { tune_pool_queue(q, spin); });
        params.work_thread_factory(factory);
        return so_5::disp::thread_pool::make_dispatcher(env, "qvo-tp", std::move(params))
            .binder(so_5::disp::thread_pool::bind_params_t{}.fifo(
                so_5::disp::thread_pool::fifo_t::individual));
    }

    so_5::disp::one_thread::disp_params_t params;
    params.tune_queue_params([spin](auto &q) { tune_queue(q, spin); });
    params.work_thread_factory(factory);
    return so_5::disp::one_thread::make_dispatcher(env, "qvo-ot", std::move(params)).binder();
}

inline std::vector<std::string> pool_caveats() {
    std::vector<std::string> c;
    push_sweep_caveat(c);
    c.insert(c.end(), {
        "cores>=2 uses the thread_pool dispatcher with exactly `cores` work threads and "
        "fifo_t::individual (one demand queue per agent, agents of one coop free to run on "
        "different threads); cores=1 uses one_thread. The work threads are pinned one per CPU "
        "from the harness's set through a custom so_5::disp::abstract_work_thread_factory_t",
        "the pool places agents dynamically: which thread runs a given agent's next demand is "
        "the dispatcher's decision, so how many hand-offs cross a core is not fixed and not "
        "reported, where qb's cell fixes actor a on core a % cores",
        "wait=1 maps to mpmc combined_lock_factory with a 10 s spin budget (never reached inside "
        "a hop); wait=0 maps to simple_lock_factory (mutex + condition variable)",
        "max_demands_at_once is SObjectizer's shipped default, 4 -- the batching knob CAF spells "
        "max-throughput=300 and qb spells draining the pipe; it was not tuned",
        "Messages derive from so_5::message_t and travel on each agent's DIRECT mbox, the "
        "framework's own fast path"});
    return c;
}

inline std::vector<std::string> caveats() {
    std::vector<std::string> c;
    push_sweep_caveat(c);
    c.insert(c.end(), {
        "cores>=2 uses the active_obj dispatcher (one work thread per agent); cores=1 uses "
        "one_thread. The work threads are pinned one per CPU from the harness's set through a "
        "custom so_5::disp::abstract_work_thread_factory_t, so SObjectizer gets the same "
        "placement qb and CAF get rather than being the one framework left floating",
        "wait=1 maps to combined_lock_factory with a 10 s spin budget (never reached inside a "
        "hop); wait=0 maps to simple_lock_factory (mutex + condition variable)",
        "Messages derive from so_5::message_t and travel on each agent's DIRECT mbox. Both of "
        "SObjectizer's shipped ping-pong samples use a shared mbox instead, which is simpler and "
        "slower; the faster idiom is used here on purpose"});
    return c;
}

}  // namespace qvoso

#endif  // QVO_SO_SUPPORT_H
