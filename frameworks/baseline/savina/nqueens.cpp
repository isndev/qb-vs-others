// @benchmark     savina/nqueens
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the master
//                is actor 0 and worker w is actor cores + w, each owned by thread id % cores.
// @idiom-note    The floor for this shape is the shared search kernel plus the cheapest relay
//                the semantics allow: a work item is the two payload words of one ring message,
//                a result and a done one word each, and the master forwards every child item to
//                the next worker actor of its rotation. Placement is static and is qb's exactly:
//                the master on thread 0, worker w on thread w % cores (its id, cores + w, is
//                chosen for that), so this floor bounds the placing frameworks and not the
//                pools, which may rebalance the search.

#include <qvospec/savina/nqueens.h>

#include "../baseline_support.h"

namespace savina_nqueens_baseline {

using namespace qvospec::savina::nqueens;

// kWork: a = w0, b = w1 of the board.  kResult: a = board_hash.  kDone: a = done_value.
enum Tag : std::uint32_t { kWork = 1, kResult = 2, kDone = 3 };

constexpr std::uint32_t kMaster = 0;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     size      = static_cast<int>(p.get("size"));
    const auto     threshold = static_cast<int>(p.get("threshold"));
    const auto     workers   = static_cast<std::uint32_t>(p.get("workers"));
    const auto     cores     = static_cast<unsigned>(p.get("cores"));
    const bool     spin      = p.get("wait") != 0;
    const unsigned W         = cores < 1 ? 1u : cores;

    // The master's state. Actor 0 is owned by thread 0, so only thread 0 ever touches it (the
    // inline drain of a full ring runs a handler re-entrantly, but on the same thread).
    std::uint32_t next      = 0;
    std::uint64_t sent      = 0;
    std::uint64_t completed = 0;
    std::uint64_t checksum  = 0;
    std::uint64_t delivered = 0;

    // Counted before the send: a send to a full ring owned by this thread drains it inline.
    auto hand_out = [&](auto &m, unsigned from, std::uint64_t w0, std::uint64_t w1) {
        const std::uint32_t to = W + next;  // worker `next`, owned by thread next % W
        if (++next == workers) next = 0;
        ++sent;
        m.send(from, qvobase::Msg{to, kWork, w0, w1});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        if (msg.dst != kMaster) {  // a worker actor: only ever sent work
            const Board item{msg.a, msg.b};
            process(
                item, size, threshold,
                [&](const Board &child) {
                    m.send(worker, qvobase::Msg{kMaster, kWork, child.w0, child.w1});
                },
                [&](std::uint64_t hash) {
                    m.send(worker, qvobase::Msg{kMaster, kResult, hash, 0});
                });
            m.send(worker, qvobase::Msg{kMaster, kDone, done_value(item), 0});
            return;
        }
        switch (msg.tag) {
        case kWork:
            ++delivered;
            hand_out(m, worker, msg.a, msg.b);
            break;
        case kResult:
            ++delivered;
            checksum += msg.a;
            break;
        case kDone:
            delivered += 2;  // this done, and the item its worker actor received
            checksum += msg.a;
            if (++completed == sent) {
                watch.stop();
                m.stop();
            }
            break;
        }
    });
    mesh.start();

    watch.start();
    const Board empty{};
    hand_out(mesh, 0, empty.w0, empty.w1);  // the calling thread is worker 0, the master's owner
    mesh.run();

    return qvo::Answer{checksum, delivered};
}

}  // namespace savina_nqueens_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::nqueens::params();
    spec.expected          = qvospec::savina::nqueens::expected;
    spec.expected_messages = qvospec::savina::nqueens::expected_messages;
    spec.work_unit         = qvospec::savina::nqueens::kWorkUnit;
    spec.work_units        = qvospec::savina::nqueens::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the master (actor 0) relays every child item round-robin to "
                             "worker actor w (id cores + w) on thread w % cores, the master on "
                             "thread 0 -- qb's placement; not an actor framework and not ranked "
                             "as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A work item is one ring message carrying the packed board, a "
        "result and a done one word each, and the search is the same shared kernel every "
        "framework runs: the floor for relaying a work-distribution search through one master",
        "worker w is owned by thread w % cores and the master by thread 0, fixed before the "
        "window -- qb's placement exactly: with cores=2 the search is split the way the "
        "rotation splits it and never rebalanced, so this floor bounds the placing frameworks "
        "and NOT the pools",
        "cores=1 is one thread with every actor's messages in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_nqueens_baseline::body);
}
