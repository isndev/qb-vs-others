// @benchmark     savina/trapezoid
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the master
//                is actor 0 and worker w is actor cores + w, each owned by thread id % cores.
// @idiom-note    The floor for this shape is the shared integration kernel plus the cheapest
//                fan-out and fan-in the semantics allow: a work message is one ring message whose
//                two payload words are the range, a result one ring message carrying the area's
//                bits and the worker. Placement is static and is qb's exactly: the master on
//                thread 0, worker w on thread w % cores (its id, cores + w, is chosen for that).

#include <qvospec/savina/trapezoid.h>

#include "../baseline_support.h"

#include <bit>
#include <vector>

namespace savina_trapezoid_baseline {

using namespace qvospec::savina::trapezoid;

// kWork: a = first trapezoid, b = one past the last.  kResult: a = the area's bits, b = the worker.
enum Tag : std::uint32_t { kWork = 1, kResult = 2 };

constexpr std::uint32_t kMaster = 0;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config   c     = config(p);
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;

    // The master's state. Actor 0 is owned by thread 0, so only thread 0 ever touches it (the
    // inline drain of a full ring runs a handler re-entrantly, but on the same thread).
    std::vector<Record> records(c.workers);
    double              total    = 0.0;
    std::uint64_t       received = 0;
    // Each worker's count, one 128-byte slot of its own, written only by the thread that owns it.
    std::vector<Tally> tallies(c.workers);

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        if (msg.dst != kMaster) {  // a worker actor: only ever sent work
            const std::uint32_t index = msg.dst - W;
            ++tallies[index].works;
            m.send(worker, qvobase::Msg{kMaster, kResult,
                                        bits(integrate(msg.a, msg.b, c.left, c.h)), index});
            return;
        }
        if (msg.b >= records.size())
            qvo::fail("savina/trapezoid: a result names worker " + std::to_string(msg.b)
                      + ", which does not exist");
        records[msg.b].add(std::bit_cast<double>(msg.a));
        if (++received == c.workers) {
            total = fold(records);
            watch.stop();
            m.stop();
        }
    });
    // A full ring the sending thread owns is drained inline, a handler running inside the send,
    // and a fan-out wider than a ring nests those drains. Every message of the run fits in one
    // ring below the bound, so no ring ever fills -- 200 against 65 536 at the defaults; above it
    // the floor declares the cell not applicable rather than run a configuration that can crash.
    if (expected_messages(p) > mesh.kRingCapacity)
        qvo::not_applicable("the run's " + std::to_string(expected_messages(p))
                            + " messages outnumber one ring of the floor ("
                            + std::to_string(mesh.kRingCapacity) + ")");
    mesh.start();

    watch.start();
    for (std::uint32_t w = 0; w < c.workers; ++w)  // the calling thread is worker 0, the master's
        mesh.send(0, qvobase::Msg{W + w, kWork, first_piece(w, c), first_piece(w + 1, c)});
    mesh.run();

    // run() joined every worker: every record and tally is final, and reduced here, outside the
    // window.
    return qvo::Answer{checksum(records, total, tallies), delivered(records, tallies)};
}

}  // namespace savina_trapezoid_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::trapezoid::params();
    spec.expected          = qvospec::savina::trapezoid::expected;
    spec.expected_messages = qvospec::savina::trapezoid::expected_messages;
    spec.work_unit         = qvospec::savina::trapezoid::kWorkUnit;
    spec.work_units        = qvospec::savina::trapezoid::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the master (actor 0, thread 0) sends one range per worker actor "
                             "w (id cores + w) on thread w % cores -- qb's placement; not an actor "
                             "framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A work message is one ring message carrying the range, a result "
        "one ring message carrying the area's bits, and the integration is the same shared "
        "kernel every framework runs: the floor for a fan-out and fan-in of pure computation",
        "worker w is owned by thread w % cores and the master by thread 0, fixed before the "
        "window -- qb's placement exactly: with cores=2 each thread integrates the ranges of the "
        "workers it owns, so this floor bounds the placing frameworks and NOT the pools",
        "cores=1 is one thread with every actor's messages in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_trapezoid_baseline::body);
}
