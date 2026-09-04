// @benchmark     savina/counting
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on raw pinned threads and the bounded SPSC ring of
//                ../baseline_support.h.
// @idiom-note    One producer, one consumer, one ring: exactly the single-producer mailbox the
//                benchmark exists to measure, with nothing around it. On one core the producer
//                fills the ring and drains it inline when it is full, which is what a
//                single-threaded actor runtime does when its own queue is the bottleneck.

#include <qvospec/savina/counting.h>

#include "../baseline_support.h"

#include <thread>

namespace savina_counting_baseline {

using namespace qvospec::savina::counting;

// The messages: the tag is the protocol, the payload is the increment index or the total.
enum Tag : std::uint32_t { kIncrement = 1, kRetrieve = 2, kResult = 3 };
constexpr std::uint32_t kProducer = 0;
constexpr std::uint32_t kCounter  = 1;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     n     = static_cast<std::uint64_t>(p.get("messages"));
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;

    std::uint64_t counter_acc   = 0;
    std::uint64_t counter_count = 0;
    std::uint64_t result        = 0;
    std::uint64_t delivered     = 0;

    // The producer is the calling thread (worker 0, the owner of actor 0); the counter is actor
    // 1, which lands on worker 1 when cores=2 and on worker 0 when cores=1.
    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kIncrement:
            counter_acc += qvo::mix(msg.a);
            ++counter_count;
            break;
        case kRetrieve:
            m.send(worker, qvobase::Msg{kProducer, kResult, counter_acc, counter_count});
            break;
        case kResult:
            result    = msg.a;
            delivered = msg.b + 2;  // the increments the counter saw, the retrieve, this result
            watch.stop();
            m.stop();
            break;
        }
    });
    mesh.start();

    watch.start();
    for (std::uint64_t i = 0; i < n; ++i)
        mesh.send(0, qvobase::Msg{kCounter, kIncrement, i, 0});
    mesh.send(0, qvobase::Msg{kCounter, kRetrieve, 0, 0});
    mesh.run();

    return qvo::Answer{result, delivered};
}

}  // namespace savina_counting_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::counting::params();
    spec.expected          = qvospec::savina::counting::expected;
    spec.expected_messages = qvospec::savina::counting::expected_messages;
    spec.work_unit         = qvospec::savina::counting::kWorkUnit;
    spec.work_units        = qvospec::savina::counting::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw std::thread + one bounded SPSC ring from producer to counter; "
                             "not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. It has no supervision, no addressing, no dynamic actor "
        "lifetime, no mailbox fairness and no backpressure beyond a full ring. It bounds how much "
        "of each framework's cost is inherent to streaming a million messages through one queue",
        "cores=1 is one thread pushing into a 65536-slot ring and draining it inline whenever it "
        "is full -- the floor for single-threaded dispatch, still a real queue",
        "the threads are pinned one per CPU from the harness's set, exactly as every framework's "
        "workers are",
        "wait=1 busy-polls the rings; wait=0 parks the consumer on a condition variable and the "
        "producer notifies it after a push only while it announces itself asleep"};

    return qvo::run(argc, argv, std::move(spec), savina_counting_baseline::body);
}
