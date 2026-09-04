// @benchmark     savina/thread-ring
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: `actors`
//                integers owned by worker actor % cores, one token hopping between them.
// @idiom-note    Every actor's state is one slot of a vector; a hop is one ring push and one
//                ring pop. With cores=2 every hop crosses a core, the same placement the qb
//                implementation gets -- so this cell is the floor for a cross-core hand-off
//                between two DIFFERENT actors each time, which is what distinguishes the ring
//                from ping-pong.

#include <qvospec/savina/thread-ring.h>

#include "../baseline_support.h"

#include <thread>

namespace savina_thread_ring_baseline {

using namespace qvospec::savina::thread_ring;

// Msg.a = remaining hops, Msg.b = the accumulator riding on the token.
enum Tag : std::uint32_t { kToken = 1, kResult = 2 };

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto     hops   = static_cast<std::uint64_t>(p.get("hops"));
    const auto     cores  = static_cast<unsigned>(p.get("cores"));
    const bool     spin   = p.get("wait") != 0;
    const unsigned W      = cores < 1 ? 1u : cores;

    // The sink is actor `actors` (owned by worker actors % W); ring actors are 0..actors-1.
    const std::uint32_t sink = actors;

    std::uint64_t result    = 0;
    std::uint64_t delivered = 0;

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        if (msg.tag == kToken) {
            const std::uint64_t remaining = msg.a;
            const std::uint64_t acc       = msg.b + qvo::mix(remaining);
            if (remaining == 1) {
                m.send(worker, qvobase::Msg{sink, kResult, acc, hops});
            } else {
                const std::uint32_t next = msg.dst + 1 == actors ? 0 : msg.dst + 1;
                m.send(worker, qvobase::Msg{next, kToken, remaining - 1, acc});
            }
        } else {
            result    = msg.a;
            delivered = msg.b + 1;  // every hop plus this result
            watch.stop();
            m.stop();
        }
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{0, kToken, hops, 0});
    mesh.run();

    return qvo::Answer{result, delivered};
}

}  // namespace savina_thread_ring_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::thread_ring::params();
    spec.expected          = qvospec::savina::thread_ring::expected;
    spec.expected_messages = qvospec::savina::thread_ring::expected_messages;
    spec.work_unit         = qvospec::savina::thread_ring::kWorkUnit;
    spec.work_units        = qvospec::savina::thread_ring::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; actor i is owned by worker i % cores; not an actor framework "
                             "and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are integers, a hop is one ring push and one ring pop, "
        "and there is no scheduler: the floor for a hand-off, not for finding the next actor",
        "actor i lives on worker i % cores, so with cores=2 every hop crosses a core -- the same "
        "placement qb gets, and the worst case for a shard-per-core design",
        "cores=1 is one thread pushing the token into its own ring and popping it back -- the "
        "floor for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "with one token and two workers means one park and one wake per hop"};

    return qvo::run(argc, argv, std::move(spec), savina_thread_ring_baseline::body);
}
