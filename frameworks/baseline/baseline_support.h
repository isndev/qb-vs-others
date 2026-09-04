// The floor's runtime, shared by every baseline implementation.
//
// NOT an actor framework (FAIRNESS.md 1.2). What is here is the least a workload's semantics
// allow: pinned raw threads, bounded single-producer/single-consumer rings between them, and a
// mutex + condition-variable park for the wait=0 column. Nothing here is taken from a framework
// under test -- a floor built from one contestant's queue is that contestant's number twice.
//
// Two shapes live here:
//
//   * SpscRing / CvSlot -- the two-thread hand-off ping-pong is built from (that file carries
//     the rationale for each). They are here so that the many-actor floor below is made of the
//     SAME ring rather than a second one that could drift from the first.
//
//   * Mesh -- the many-actor floor. W pinned workers, one SPSC ring per (producer, consumer)
//     pair, every "actor" a small integer owned by worker actor % W, and a consumer that polls
//     its W inbound rings round-robin. That is the cheapest static-placement message fabric that
//     preserves the frameworks' guarantees: per-sender FIFO, no lost wake-up, no message ever
//     dropped under a full ring. It is the shape of qb's own core-to-core pipes, written
//     independently, and it is deliberately NOT a work-stealing scheduler: a floor exists to
//     bound the cost of moving a message between two threads, not to compete on load balance.

#ifndef QVO_BASELINE_SUPPORT_H
#define QVO_BASELINE_SUPPORT_H

#include <qvo/harness.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace qvobase {

// Not std::hardware_destructive_interference_size: g++ 12+ warns on every use of it
// (-Winterference-size) because its value is a compile-flag, not a target fact, and the floor
// must build warning-free on every toolchain the report cites. 128 on arm64 (Apple M-series
// prefetches pairs of 64-byte lines), 64 everywhere else measured here.
#if defined(__aarch64__) || defined(_M_ARM64)
constexpr std::size_t kCacheLine = 128;
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

    // Consumer side only: is there anything to pop. Used by the parked consumer to decide
    // whether it may sleep, AFTER it has announced that it is about to.
    bool empty() const noexcept {
        return _tail.load(std::memory_order_relaxed) == _head.load(std::memory_order_acquire);
    }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> _head{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> _tail{0};
    alignas(kCacheLine) T _slots[N]{};
};

// The parked counterpart of the ring: a mutex + condition variable hand-off, which is what every
// framework's wait=0 configuration reduces to underneath. It is here so that the floor exists on
// BOTH sides of the spin/park axis -- a floor that only exists for spinning would let the parked
// column be read against nothing.
template <typename T>
class CvSlot {
public:
    void put(T v) {
        {
            std::lock_guard<std::mutex> lk(_m);
            _value = v;
            _full  = true;
        }
        _cv.notify_one();
    }

    T take() {
        std::unique_lock<std::mutex> lk(_m);
        _cv.wait(lk, [this] { return _full; });
        _full = false;
        return _value;
    }

private:
    std::mutex              _m;
    std::condition_variable _cv;
    T                       _value{};
    bool                    _full{false};
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

// The message of the many-actor floor. Deliberately the shape of the frameworks' events: a
// destination, a tag, and two 64-bit payload words -- 24 bytes, one cache line holds two.
struct Msg {
    std::uint32_t dst{0};
    std::uint32_t tag{0};
    std::uint64_t a{0};
    std::uint64_t b{0};
};

// The many-actor floor.
//
//   qvobase::Mesh mesh(workers, spin, [&](auto &mesh, unsigned worker, const qvobase::Msg &m) {
//       ...                       // runs on `worker`, the owner of m.dst
//       mesh.send(worker, next);  // from a handler, name the worker you are on
//   });
//   mesh.start();                 // workers 1..W-1 spawn, pin, and poll (or park)
//   watch.start();
//   mesh.send(0, first);          // the calling thread IS worker 0 until run() returns
//   mesh.run();                   // runs worker 0 here; returns once a handler called stop()
//
// Delivery: the handler for actor d runs on worker d % W, and messages from one worker to one
// actor arrive in the order they were sent (one ring per pair). A message to a FULL ring owned
// by the sending worker itself is resolved by draining the sender's own inbound rings inline --
// that is what a single-threaded actor runtime does when its own queue is the bottleneck, and
// without it a one-core fan-out of more than one ring's worth of messages would deadlock. A full
// ring to ANOTHER worker spins until the consumer makes room; in park mode the consumer was
// woken by the first push, so it is making room.
//
// Wake-up (wait=0): the consumer announces `sleeping` (seq_cst), re-checks every inbound ring,
// and only then waits under the lock; the producer pushes, fences (seq_cst), and notifies if it
// sees `sleeping`. One of the two always observes the other's write, and the wait's predicate
// re-checks the rings under the same lock the notifier takes, so no wake-up is lost.
template <typename Handler>
class Mesh {
public:
    static constexpr std::size_t kRingCapacity = std::size_t{1} << 16;
    using Ring                                 = SpscRing<Msg, kRingCapacity>;

    Mesh(unsigned workers, bool spin, Handler handler)
        : _workers(workers == 0 ? 1u : workers)
        , _spin(spin)
        , _handler(std::move(handler))
        , _rings(static_cast<std::size_t>(_workers) * _workers)
        , _parks(_workers) {
        for (auto &r : _rings) r = std::make_unique<Ring>();
    }

    Mesh(const Mesh &)            = delete;
    Mesh &operator=(const Mesh &) = delete;

    unsigned workers() const noexcept { return _workers; }
    unsigned owner_of(std::uint32_t actor) const noexcept { return actor % _workers; }

    // Spawn workers 1..W-1. Worker 0 is the calling thread, pinned here and run by run().
    void start() {
        place(0);
        for (unsigned w = 1; w < _workers; ++w)
            _threads.emplace_back([this, w] {
                place(w);
                loop(w);
            });
    }

    // Deliver `m` to the worker owning m.dst, from worker `from` (the thread calling this).
    void send(unsigned from, const Msg &m) noexcept {
        const unsigned to = owner_of(m.dst);
        Ring          &r  = ring(from, to);
        while (!r.try_push(m)) {
            if (to == from) drain(from);
            // else: spin; the consumer is awake (see the class comment) and draining.
        }
        if (!_spin) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            Park &p = _parks[to];
            if (p.sleeping.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lk(p.m);
                p.cv.notify_one();
            }
        }
    }

    // From a handler: end the run. Every worker leaves its loop once its inbound rings are
    // empty, and run() joins them.
    void stop() noexcept {
        _stop.store(true, std::memory_order_seq_cst);
        if (!_spin)
            for (auto &p : _parks) {
                std::lock_guard<std::mutex> lk(p.m);
                p.cv.notify_all();
            }
    }

    // Run worker 0 on the calling thread until stop() has been called, then join the others.
    void run() {
        loop(0);
        for (auto &t : _threads) t.join();
        _threads.clear();
    }

private:
    struct alignas(kCacheLine) Park {
        std::mutex              m;
        std::condition_variable cv;
        std::atomic<bool>       sleeping{false};
    };

    Ring &ring(unsigned from, unsigned to) noexcept {
        return *_rings[static_cast<std::size_t>(from) * _workers + to];
    }

    // Pop everything currently queued for `w`, one ring at a time. Returns whether anything ran.
    bool drain(unsigned w) {
        bool any = false;
        Msg  m;
        for (unsigned src = 0; src < _workers; ++src) {
            Ring &r = ring(src, w);
            while (r.try_pop(m)) {
                _handler(*this, w, m);
                any = true;
            }
        }
        return any;
    }

    bool pending(unsigned w) const noexcept {
        for (unsigned src = 0; src < _workers; ++src)
            if (!_rings[static_cast<std::size_t>(src) * _workers + w]->empty()) return true;
        return false;
    }

    void loop(unsigned w) {
        for (;;) {
            if (drain(w)) continue;
            if (_stop.load(std::memory_order_relaxed)) return;
            if (!_spin) park(w);
        }
    }

    void park(unsigned w) {
        Park &p = _parks[w];
        p.sleeping.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!pending(w) && !_stop.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(p.m);
            p.cv.wait(lk, [this, w] {
                return pending(w) || _stop.load(std::memory_order_relaxed);
            });
        }
        p.sleeping.store(false, std::memory_order_seq_cst);
    }

    unsigned                           _workers;
    bool                               _spin;
    Handler                            _handler;
    std::vector<std::unique_ptr<Ring>> _rings;  // [from * W + to]
    std::vector<Park>                  _parks;
    std::vector<std::thread>           _threads;
    std::atomic<bool>                  _stop{false};
};

template <typename Handler>
Mesh(unsigned, bool, Handler) -> Mesh<Handler>;

}  // namespace qvobase

#endif  // QVO_BASELINE_SUPPORT_H
