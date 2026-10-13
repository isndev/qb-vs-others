// @benchmark     savina/recmatmul
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the master
//                is actor 0 and worker w is actor cores x (w + 1) + floor(w x cores / workers),
//                owned by thread id % cores = floor(w x cores / workers).
// @idiom-note    The floor for this shape is the shared leaf kernel plus the cheapest relay the
//                semantics allow: a task is one ring message whose two payload words carry the
//                eight 32-bit fields of a Task (the corners of A, B and C, the block count and the
//                dimension, sixteen bits each), a done one ring message with no payload, and the
//                master forwards every created task to the worker Savina's routing names.
//                Placement is static and is qb's exactly: the master on thread 0, worker w on
//                thread floor(w x cores / workers).

#include <qvospec/savina/recmatmul.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_recmatmul_baseline {

using namespace qvospec::savina::recmatmul;

// kWork: a = srA | scA << 16 | srB << 32 | scB << 48, b = srC | scC << 16 | dim << 32 (the block
// count is dim^2, recomputed).  kDone: no payload. Every field is below 2^16: length <= 2 048.
enum Tag : std::uint32_t { kWork = 1, kDone = 2 };

constexpr std::uint32_t kMaster = 0;

inline qvobase::Msg encode(std::uint32_t dst, const Task &t) noexcept {
    const std::uint64_t a = t.srA | static_cast<std::uint64_t>(t.scA) << 16
                            | static_cast<std::uint64_t>(t.srB) << 32
                            | static_cast<std::uint64_t>(t.scB) << 48;
    const std::uint64_t b = t.srC | static_cast<std::uint64_t>(t.scC) << 16
                            | static_cast<std::uint64_t>(t.dim) << 32;
    return qvobase::Msg{dst, kWork, a, b};
}
inline Task decode(const qvobase::Msg &m) noexcept {
    Task t;
    t.srA    = static_cast<std::uint32_t>(m.a & 0xffff);
    t.scA    = static_cast<std::uint32_t>((m.a >> 16) & 0xffff);
    t.srB    = static_cast<std::uint32_t>((m.a >> 32) & 0xffff);
    t.scB    = static_cast<std::uint32_t>((m.a >> 48) & 0xffff);
    t.srC    = static_cast<std::uint32_t>(m.b & 0xffff);
    t.scC    = static_cast<std::uint32_t>((m.b >> 16) & 0xffff);
    t.dim    = static_cast<std::uint32_t>(m.b >> 32);
    t.blocks = t.dim * t.dim;
    return t;
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config   c     = config(p);
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;

    auto id_of = [&](std::uint32_t w) {
        return W * (w + 1) + core_of(w, c.workers, W);  // owned by thread core_of(w)
    };

    Matrices matrices(c.length);  // A, B and a zero C, built before the threads
    // The master's counts: actor 0 is owned by thread 0, so only thread 0 ever touches them (the
    // inline drain of a full ring runs a handler re-entrantly, but on the same thread).
    MasterCounts counts;
    // Each worker's counts, one 128-byte slot of its own, written only by the thread that owns it.
    std::vector<Tally> tallies(c.workers);

    auto send_task = [&](auto &m, unsigned from, const Task &task) {
        ++counts.sent;
        m.send(from, encode(id_of(route(task, c)), task));
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned thread, const qvobase::Msg &msg) {
        if (msg.dst != kMaster) {  // a worker actor: only ever sent work
            Tally     &t    = tallies[msg.dst / W - 1];
            const Task task = decode(msg);
            ++t.works;
            if (is_leaf(task, c)) {
                ++t.leaves;
                multiply(matrices, task);
            } else {
                ++t.splits;
                split(task, [&](const Task &child) { m.send(thread, encode(kMaster, child)); });
            }
            m.send(thread, qvobase::Msg{kMaster, kDone, 0, 0});
            return;
        }
        if (msg.tag == kWork) {  // a task a worker created: relayed to its worker
            ++counts.created;
            send_task(m, thread, decode(msg));
            return;
        }
        if (++counts.dones == counts.sent) {
            watch.stop();
            m.stop();
        }
    });
    // A full ring the sending thread owns is drained inline, a handler running inside the send:
    // a tree whose messages outnumber one ring nests those drains as deep as the tree is wide and
    // can exhaust the thread's stack (measured: length=128 threshold=4, 299 593 tasks). Every
    // message of the run fits in one ring below the bound, so no ring ever fills -- 1 754 against
    // 65 536 at the defaults; above it the floor declares the cell not applicable rather than run
    // a configuration that can crash.
    if (expected_messages(p) > mesh.kRingCapacity)
        qvo::not_applicable("the run's " + std::to_string(expected_messages(p))
                            + " messages outnumber one ring of the floor ("
                            + std::to_string(mesh.kRingCapacity)
                            + "): a full ring the sending thread owns is drained inline, and a "
                              "tree that wide nests the drains until the stack runs out");
    mesh.start();

    watch.start();
    send_task(mesh, 0, root(c));  // the calling thread is worker 0, the master's owner
    mesh.run();

    // run() joined every worker: C, the tallies and the master's counts are final, and reduced
    // here, outside the window.
    return qvo::Answer{checksum(matrices, tallies, counts), delivered(tallies, counts)};
}

}  // namespace savina_recmatmul_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::recmatmul::params();
    spec.expected          = qvospec::savina::recmatmul::expected;
    spec.expected_messages = qvospec::savina::recmatmul::expected_messages;
    spec.work_unit         = qvospec::savina::recmatmul::kWorkUnit;
    spec.work_units        = qvospec::savina::recmatmul::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the master (actor 0, thread 0) relays every created task to "
                             "worker (srC + scC) % workers on thread floor(w x cores / workers) -- "
                             "qb's placement; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A task is one ring message carrying the Task's fields, a done "
        "one ring message, and every leaf is the same shared kernel every framework runs: the "
        "floor for a recursive decomposition relayed through one master",
        "worker w is owned by thread floor(w x cores / workers) and the master by thread 0, fixed "
        "before the window -- qb's placement exactly: with cores=2 thread 0 multiplies 304 of the "
        "default's 512 leaves and thread 1 208, never rebalanced, so this floor bounds the placing "
        "frameworks and NOT the pools",
        "cores=1 is one thread with every actor's messages in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_recmatmul_baseline::body);
}
