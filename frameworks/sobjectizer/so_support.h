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
#include <memory>
#include <mutex>
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
template <typename QueueParams>
void tune_queue(QueueParams &q, bool spin) {
    if (spin)
        q.lock_factory(
            so_5::disp::mpsc_queue_traits::combined_lock_factory(std::chrono::seconds{10}));
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

inline std::vector<std::string> caveats() {
    return {
        "cores>=2 uses the active_obj dispatcher (one work thread per agent); cores=1 uses "
        "one_thread. The work threads are pinned one per CPU from the harness's set through a "
        "custom so_5::disp::abstract_work_thread_factory_t, so SObjectizer gets the same "
        "placement qb and CAF get rather than being the one framework left floating",
        "wait=1 maps to combined_lock_factory with a 10 s spin budget (never reached inside a "
        "hop); wait=0 maps to simple_lock_factory (mutex + condition variable)",
        "Messages derive from so_5::message_t and travel on each agent's DIRECT mbox. Both of "
        "SObjectizer's shipped ping-pong samples use a shared mbox instead, which is simpler and "
        "slower; the faster idiom is used here on purpose"};
}

}  // namespace qvoso

#endif  // QVO_SO_SUPPORT_H
