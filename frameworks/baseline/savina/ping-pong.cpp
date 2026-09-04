// @benchmark     savina/ping-pong
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. This is the workload written with raw threads and a hand-written bounded
//                SPSC ring, doing the minimum the benchmark's semantics allow.
// @idiom-note    The ring is written HERE rather than taken from any framework under test. Using
//                qb's `qb::lockfree::spsc::ringbuffer` would have made the floor one competitor's
//                own code, and a floor that belongs to a contestant is not a floor.
//
// What this target is for (FAIRNESS.md 1.2): without it, a table in which every framework is slow
// reads as a win for whoever is least slow. With it, the table shows how much of each framework's
// cost is inherent to passing a message between two threads at all, and how much is the framework.

#include <qvospec/savina/ping-pong.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace savina_ping_pong_baseline {

using namespace qvospec::savina::ping_pong;

#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;
#endif

// A textbook bounded single-producer/single-consumer ring. Head and tail sit on separate cache
// lines; the producer never reads the consumer's cursor except to check fullness, and vice versa.
template <typename T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

public:
    bool try_push(T value) noexcept {
        const auto head = _head.load(std::memory_order_relaxed);
        const auto next = head + 1;
        if (next - _tail.load(std::memory_order_acquire) > N) return false;
        _slots[head & (N - 1)] = value;
        _head.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T &out) noexcept {
        const auto tail = _tail.load(std::memory_order_relaxed);
        if (tail == _head.load(std::memory_order_acquire)) return false;
        out = _slots[tail & (N - 1)];
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> _head{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> _tail{0};
    alignas(kCacheLine) T _slots[N]{};
};

// The message. Deliberately the same shape as the frameworks' events: one 64-bit payload.
struct Ball {
    std::uint64_t seq{0};
};

// The floor gets the SAME placement as the frameworks.
//
// qb pins its VirtualCores, CAF pins its scheduler workers through a thread_hook and SObjectizer
// pins its work threads through a factory. A floor left floating on the process mask while every
// framework is pinned is measured under different conditions -- and since the floor is what every
// framework's number is divided by, that would move every published ratio.
inline void place(std::size_t worker_index) {
    const auto &cpus = qvo::pinned_cpus();
    if (!cpus.empty()) qvo::pin_this_thread(cpus[worker_index % cpus.size()]);
}

// The parked counterpart of the ring: a mutex + condition variable handoff, which is what every
// framework's wait=0 configuration reduces to underneath. It is here so that the floor exists on
// BOTH sides of the spin/park axis -- a floor that only exists for spinning would let the parked
// column be read against nothing.
class CvSlot {
public:
    void put(Ball b) {
        {
            std::lock_guard<std::mutex> lk(_m);
            _value = b;
            _full  = true;
        }
        _cv.notify_one();
    }

    Ball take() {
        std::unique_lock<std::mutex> lk(_m);
        _cv.wait(lk, [this] { return _full; });
        _full = false;
        return _value;
    }

private:
    std::mutex              _m;
    std::condition_variable _cv;
    Ball                    _value{};
    bool                    _full{false};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds = static_cast<std::uint64_t>(p.get("messages"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    std::uint64_t acc       = 0;
    std::uint64_t delivered = 0;

    place(0);  // the calling thread plays the ping role

    if (cores <= 1) {
        // One thread, both roles, but the message still goes THROUGH A QUEUE.
        //
        // An earlier version of this branch just copied the value and called it a pong. That is
        // the cost of the arithmetic and nothing else -- it measured 0.55 ns per round trip and
        // would have made every framework look 200x worse than a floor that was not doing the
        // job. A single-core actor framework still enqueues, dequeues and dispatches; the floor
        // for that is a real queue, not an assignment.
        SpscRing<Ball, 1024> mailbox;

        watch.start();
        for (std::uint64_t seq = rounds; seq-- > 0;) {
            // ping -> pong
            while (!mailbox.try_push(Ball{seq})) { /* unreachable at depth 1 */ }
            Ball inbound{};
            while (!mailbox.try_pop(inbound)) { /* unreachable */ }
            // pong -> ping
            while (!mailbox.try_push(inbound)) { /* unreachable */ }
            Ball back{};
            while (!mailbox.try_pop(back)) { /* unreachable */ }

            acc += qvo::mix(back.seq);
            delivered += 2;
        }
        watch.stop();
        return qvo::Answer{acc, delivered};
    }

    if (!spin) {
        // Two threads, parked on condition variables.
        CvSlot to_pong_slot;
        CvSlot to_ping_slot;

        std::thread pong([&] {
            place(1);
            for (;;) {
                const Ball b = to_pong_slot.take();
                to_ping_slot.put(b);
                if (b.seq == 0) return;
            }
        });

        watch.start();
        to_pong_slot.put(Ball{rounds - 1});
        for (;;) {
            const Ball back = to_ping_slot.take();
            acc += qvo::mix(back.seq);
            delivered += 2;
            if (back.seq == 0) break;
            to_pong_slot.put(Ball{back.seq - 1});
        }
        watch.stop();

        pong.join();
        return qvo::Answer{acc, delivered};
    }

    // Two threads, busy-spinning on two SPSC rings -- the same reference shape qb's own
    // ping-pong benchmark uses for its floor, written independently here.
    SpscRing<Ball, 1024> to_pong;
    SpscRing<Ball, 1024> to_ping;
    std::atomic<bool>    running{true};

    std::thread pong([&] {
        place(1);
        Ball b{};
        while (running.load(std::memory_order_relaxed)) {
            if (to_pong.try_pop(b))
                while (!to_ping.try_push(b)) { /* spin */ }
        }
    });

    watch.start();
    {
        Ball b{rounds - 1};
        while (!to_pong.try_push(b)) { /* spin */ }

        for (;;) {
            Ball back{};
            while (!to_ping.try_pop(back)) { /* spin */ }
            acc += qvo::mix(back.seq);
            delivered += 2;
            if (back.seq == 0) break;
            const Ball next{back.seq - 1};
            while (!to_pong.try_push(next)) { /* spin */ }
        }
    }
    watch.stop();

    running.store(false, std::memory_order_relaxed);
    pong.join();

    return qvo::Answer{acc, delivered};
}

}  // namespace savina_ping_pong_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::ping_pong::params();
    spec.expected          = qvospec::savina::ping_pong::expected;
    spec.expected_messages = qvospec::savina::ping_pong::expected_messages;
    spec.work_unit         = qvospec::savina::ping_pong::kWorkUnit;
    spec.work_units        = qvospec::savina::ping_pong::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw std::thread + an independently written bounded SPSC ring, "
                             "busy-spinning; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. It has no supervision, no addressing, no dynamic actor "
        "lifetime, no mailbox fairness and no backpressure. It exists to bound how much of each "
        "framework's cost is inherent to the workload rather than to the framework",
        "cores=1 is one thread passing each message through a real queue in both directions -- "
        "the floor for single-threaded dispatch, not a bare assignment",
        "the two threads are pinned one per CPU from the harness's set, exactly as every "
        "framework's workers are -- a floating floor would move every ratio computed against it",
        "wait=1 is two threads busy-spinning on SPSC rings; wait=0 is two threads parked on "
        "condition variables. Both floors exist so that neither column of the spin/park axis is "
        "published without one"};

    return qvo::run(argc, argv, std::move(spec), savina_ping_pong_baseline::body);
}
