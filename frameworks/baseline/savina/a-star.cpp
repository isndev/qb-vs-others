// @benchmark     savina/a-star
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the master
//                is actor 0 and search worker w is actor w + 1, each owned by thread a % cores.
// @idiom-note    A work message, a handed-back node and an acknowledgement are each one ring push
//                and one ring pop; a worker is a slot holding its search queue, the master a few
//                counters on thread 0. The search itself -- the graph, the claims, the busy work
//                -- is the spec's, the same object code every framework runs. What a framework
//                adds on top is a mailbox per actor, a registry and its own scheduler's placement.

#include <qvospec/savina/a-star.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_a_star_baseline {

using namespace qvospec::savina::a_star;

// kWork: dst = a worker, a = node.  kBack: dst = the master, a = node handed back.
// kAck: dst = the master, a = chk, b = nodes searched.
enum Tag : std::uint32_t { kWork = 1, kBack = 2, kAck = 3 };

constexpr std::uint32_t kMaster = 0;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto     threshold = static_cast<std::uint32_t>(p.get("threshold"));
    const auto     work      = static_cast<int>(p.get("work"));
    const auto     cores     = static_cast<unsigned>(p.get("cores"));
    const bool     spin      = p.get("wait") != 0;
    const unsigned W         = cores < 1 ? 1u : cores;

    const Grid grid(static_cast<std::uint32_t>(p.get("grid")));
    Claims     claims(grid.nodes());

    // Search worker w's queue, touched only by the thread owning actor w + 1.
    std::vector<std::vector<std::uint32_t>> queues(workers);

    // The master's state, touched only by thread 0 (the calling thread until run() returns).
    std::uint64_t sent      = 0;
    std::uint64_t completed = 0;
    std::uint64_t result    = 0;
    std::uint64_t delivered = 0;

    auto dispatch = [&](auto &m, std::uint32_t node) {
        const auto to = static_cast<std::uint32_t>(1 + sent++ % workers);
        m.send(0, qvobase::Msg{to, kWork, node, 0});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kWork: {
            // Taken out of its slot for the search: a send into this thread's own ring when it is
            // full drains that ring inline (baseline_support.h), which can run another work
            // message for the SAME worker before `emit` returns -- the inner search then finds an
            // empty slot and allocates its own queue instead of clearing the one being emitted.
            // Unreachable at the default grid -- a work or handed-back message carries a node not
            // yet searched and an acknowledgement a work message, so at most 2 x 20 515 are in
            // flight against a 65 536 ring -- and reachable at a larger one.
            std::vector<std::uint32_t> queue = std::move(queues[msg.dst - 1]);
            const auto                 root  = static_cast<std::uint32_t>(msg.a);
            const Chunk c = search(grid, claims, root, threshold, work, queue,
                                   [&m, worker](std::uint32_t node) {
                                       m.send(worker, qvobase::Msg{kMaster, kBack, node, 0});
                                   });
            queues[msg.dst - 1] = std::move(queue);
            m.send(worker, qvobase::Msg{kMaster, kAck, c.chk, c.nodes});
            break;
        }
        case kBack:
            ++delivered;
            dispatch(m, static_cast<std::uint32_t>(msg.a));
            break;
        case kAck:
            // The acknowledgement and the work message it proves delivered.
            delivered += 2;
            result += msg.a;
            if (++completed == sent) {
                watch.stop();
                m.stop();
            }
            break;
        }
    });
    mesh.start();

    watch.start();
    dispatch(mesh, Grid::kOrigin);
    mesh.run();

    return qvo::Answer{result, delivered};
}

}  // namespace savina_a_star_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::a_star::params();
    spec.expected          = qvospec::savina::a_star::expected;
    spec.work_unit         = qvospec::savina::a_star::kWorkUnit;
    spec.work_units        = qvospec::savina::a_star::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; master = actor 0, search worker w = actor w + 1, actor a owned "
                             "by thread a % cores; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are integers and there is no per-actor mailbox: a "
        "thread's inbound ring is shared by every actor it owns, and the search -- graph, claims, "
        "busy work -- is the spec's own code, the same every framework runs",
        "actor a lives on thread a % cores (master = 0, search worker w = w + 1), the placement qb "
        "has: with cores=2 a node handed back crosses a thread whenever its next worker lives on "
        "the other one, and nothing rebalances beyond the master's round-robin",
        "the claim slots are shared memory every worker CASes, as in Savina's own implementation "
        "(a-star.h, Claims)",
        "cores=1 is one thread with every actor in its own ring -- the floor for single-threaded "
        "dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_a_star_baseline::body);
}
