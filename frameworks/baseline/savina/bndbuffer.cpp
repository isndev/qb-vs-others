// @benchmark     savina/bndbuffer
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the
//                manager, the producers and the consumers are fixed ids, actor a owned by worker
//                a % cores, each one's state touched only by the thread that owns it.
// @idiom-note    The floor does what the semantics require and nothing else: Savina's protocol
//                with the manager's three lists as rings (the spec's Fifo, the same code every
//                adapter's manager runs) and every message one ring push. Actor 0 is the manager,
//                1 + i producer i, 1 + producers + j consumer j -- so with cores=2 the manager is
//                on worker 0 and each kind is split evenly over the two workers, the placement
//                qb's cell has. A producer or a consumer is a cache-line-aligned slot, so two of
//                them owned by different workers never share a line. A producer that has sent
//                its exit is ended and handles nothing more, as the frameworks' producers end.

#include <qvospec/savina/bndbuffer.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_bndbuffer_baseline {

using namespace qvospec::savina::bndbuffer;

// kProduce: a = the request's sequence number. kData: a = key(producer, index), b = the value.
// kAvailable: a = the consumer's delta, b = consumer << 32 | messages it received since its last.
// kDone: a = the producer's receipts, b = producer << 32 | the requests it received.
enum Tag : std::uint32_t { kProduce = 1, kData, kAvailable, kDone };

constexpr std::uint32_t kManager = 0;

inline std::uint64_t pack(std::uint32_t high, std::uint64_t low) noexcept {
    return (static_cast<std::uint64_t>(high) << 32) | (low & 0xffffffffULL);
}

struct alignas(qvobase::kCacheLine) ProducerState {
    std::uint64_t value{0};  // the chain (spec: produce)
    std::uint32_t produced{0};
    bool          done{false};  // sent its exit: ended, it handles nothing more
    std::uint64_t fold{0};
    std::uint64_t received{0};
};

struct alignas(qvobase::kCacheLine) ConsumerState {
    std::uint64_t received{0};
    std::uint64_t reported{0};
};

struct ManagerState {
    Fifo<Item>                 buffer;
    Fifo<std::uint32_t>        available;  // consumers, oldest first
    Fifo<std::uint32_t>        parked;     // producers, oldest first
    std::vector<std::uint32_t> requests;   // ProduceData sent, per producer
    std::uint64_t              acc{0};     // the manager's terms of the checksum (bndbuffer.h)
    std::uint64_t              received{0};
    std::uint64_t              reported{0};  // messages counted by producers and consumers
    std::uint32_t              ended{0};
    std::uint64_t              producer_waits{0};
    std::uint64_t              consumer_waits{0};
    std::uint64_t              peak{0};
    std::uint64_t              overflows{0};
    bool                       done{false};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Counts        counts     = qvospec::savina::bndbuffer::counts(p);
    const int           prod_iters = iterations_of(p.get("prod_cost"));
    const int           cons_iters = iterations_of(p.get("cons_cost"));
    const std::uint64_t limit      = threshold(counts);
    const auto          cores      = static_cast<unsigned>(p.get("cores"));
    const bool          spin       = p.get("wait") != 0;
    const unsigned      W          = cores < 1 ? 1u : cores;

    const std::uint32_t first_consumer = 1 + counts.producers;
    auto producer_id = [](std::uint32_t i) { return 1 + i; };
    auto consumer_id = [first_consumer](std::uint32_t j) { return first_consumer + j; };

    ManagerState mgr;
    mgr.buffer    = Fifo<Item>(static_cast<std::size_t>(counts.buffer));
    mgr.available = Fifo<std::uint32_t>(counts.consumers);
    mgr.parked    = Fifo<std::uint32_t>(counts.producers);
    mgr.requests.assign(counts.producers, 0);
    std::vector<ProducerState> producers(counts.producers);
    std::vector<ConsumerState> consumers(counts.consumers);
    for (std::uint32_t i = 0; i < counts.producers; ++i) producers[i].value = producer_seed(i);

    std::uint64_t checksum = 0;
    std::uint64_t messages = 0;

    // The manager's moves, run on worker 0 -- the manager's owner -- from a handler or, before
    // the loop, from the calling thread, which IS worker 0.
    auto request = [&](auto &m, unsigned worker, std::uint32_t producer) {
        m.send(worker, qvobase::Msg{producer_id(producer), kProduce, ++mgr.requests[producer], 0});
    };
    auto hand = [&](auto &m, unsigned worker, std::uint32_t consumer, const Item &item) {
        mgr.acc += route(consumer, item.producer, item.index);
        m.send(worker, qvobase::Msg{consumer_id(consumer), kData, key(item.producer, item.index),
                                    item.value});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        // The reference's tryExit: every producer done and every consumer available. At-least
        // tests, identical to the reference's on a correct run: a run that duplicated a message
        // can step past either count, and it must end with its wrong checksum rather than wait.
        auto try_exit = [&] {
            if (mgr.done || mgr.ended < counts.producers || mgr.available.size() < counts.consumers)
                return;
            mgr.done = true;
            watch.stop();
            checksum = mgr.acc + overflow_weight() * mgr.overflows;
            messages = mgr.reported + mgr.received;
            m.stop();
        };

        switch (msg.tag) {
        case kProduce: {  // at a producer
            const std::uint32_t number = msg.dst - 1;
            ProducerState      &s      = producers[number];
            if (s.done) break;  // ended: a request that reaches it now changes nothing
            ++s.received;
            s.fold += receipt(number, static_cast<std::uint32_t>(msg.a));
            if (s.produced == counts.items) {  // asked once more after the last item: done
                m.send(worker, qvobase::Msg{kManager, kDone, s.fold, pack(number, s.received)});
                s.done = true;
                break;
            }
            s.value = produce(s.value, number, s.produced, prod_iters);
            m.send(worker, qvobase::Msg{kManager, kData, key(number, s.produced), s.value});
            ++s.produced;
            break;
        }
        case kData: {
            const auto producer = static_cast<std::uint32_t>(msg.a >> 32);
            const auto index    = static_cast<std::uint32_t>(msg.a & 0xffffffffULL);
            if (msg.dst == kManager) {
                ++mgr.received;
                const Item item{producer, index, msg.b};
                if (mgr.available.empty()) {
                    mgr.buffer.push(item);
                    if (mgr.buffer.size() > mgr.peak) mgr.peak = mgr.buffer.size();
                    if (mgr.buffer.size() >= counts.buffer) ++mgr.overflows;  // the protocol cannot
                } else {
                    hand(m, worker, mgr.available.pop(), item);
                }
                if (mgr.buffer.size() >= limit) {
                    mgr.parked.push(producer);
                    ++mgr.producer_waits;
                } else {
                    request(m, worker, producer);
                }
                if (!bound_holds(mgr.buffer.size(), mgr.parked.size(), mgr.ended, counts))
                    ++mgr.overflows;
            } else {  // at a consumer
                const std::uint32_t number = msg.dst - first_consumer;
                ConsumerState      &s      = consumers[number];
                ++s.received;
                const std::uint64_t delta =
                    item_term(producer, index, msg.b, cons_iters) - route(number, producer, index);
                m.send(worker, qvobase::Msg{kManager, kAvailable, delta,
                                            pack(number, s.received - s.reported)});
                s.reported = s.received;
            }
            break;
        }
        case kAvailable: {  // at the manager
            ++mgr.received;
            mgr.acc += msg.a;
            mgr.reported += msg.b & 0xffffffffULL;
            const auto consumer = static_cast<std::uint32_t>(msg.b >> 32);
            if (mgr.buffer.empty()) {
                mgr.available.push(consumer);
                ++mgr.consumer_waits;
                try_exit();
            } else {
                hand(m, worker, consumer, mgr.buffer.pop());
                if (!mgr.parked.empty()) request(m, worker, mgr.parked.pop());
            }
            break;
        }
        case kDone: {  // at the manager
            ++mgr.received;
            mgr.acc += msg.a;
            mgr.reported += msg.b & 0xffffffffULL;
            ++mgr.ended;
            try_exit();
            break;
        }
        }
    });
    mesh.start();

    // Every consumer available before the window, as the other adapters do in their last
    // handshake handler; then the reference's onPostStart -- every producer asked -- opens it.
    for (std::uint32_t c = 0; c < counts.consumers; ++c) mgr.available.push(c);
    watch.start();
    for (std::uint32_t i = 0; i < counts.producers; ++i) request(mesh, 0, i);
    mesh.run();

    qvo::Answer answer{checksum, messages};
    answer.observed[kObservedProducerWaits] = mgr.producer_waits;
    answer.observed[kObservedConsumerWaits] = mgr.consumer_waits;
    answer.observed[kObservedBufferPeak]    = mgr.peak;
    return answer;
}

}  // namespace savina_bndbuffer_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bndbuffer::params();
    spec.expected          = qvospec::savina::bndbuffer::expected;
    spec.expected_messages = qvospec::savina::bndbuffer::expected_messages;
    spec.work_unit         = qvospec::savina::bndbuffer::kWorkUnit;
    spec.work_units        = qvospec::savina::bndbuffer::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; Savina's protocol with the manager's lists as rings; actor a "
                             "on worker a % cores (manager 0, producers 1.., consumers after); "
                             "not an actor framework";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Every actor is a fixed id with a cache-line slot, every message "
        "one ring push: the floor for what this coordination costs",
        "the manager is on worker 0, producer i on worker (1 + i) % cores and consumer j on worker "
        "(1 + producers + j) % cores -- the static placement qb's cell has, so this floor bounds "
        "the placing frameworks and NOT the pools",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_bndbuffer_baseline::body);
}
