// @benchmark     savina/fork-join
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: `actors`
//                workers owned by worker w % cores, a master streaming jobs at them round-robin.
// @idiom-note    The master is the calling thread (worker 0). Half its jobs go into its own ring
//                and are drained inline when it fills, half cross to worker 1 -- the same split
//                the qb implementation gets from the same placement rule.

#include <qvospec/savina/fork-join.h>

#include "../baseline_support.h"

#include <thread>
#include <vector>

namespace savina_fork_join_baseline {

using namespace qvospec::savina::fork_join;

// kJob: a = job index i. kDone: a = the worker's accumulator, b = jobs it received.
enum Tag : std::uint32_t { kJob = 1, kDone = 2 };

struct Worker {
    std::uint64_t acc{0};
    std::uint64_t received{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     actors   = static_cast<std::uint32_t>(p.get("actors"));
    const auto     messages = static_cast<std::uint64_t>(p.get("messages"));
    const auto     work     = static_cast<int>(p.get("work"));
    const auto     cores    = static_cast<unsigned>(p.get("cores"));
    const bool     spin     = p.get("wait") != 0;
    const unsigned W        = cores < 1 ? 1u : cores;

    const std::uint32_t master = actors;  // the sink actor; workers are 0..actors-1

    std::vector<Worker> workers(actors);
    std::uint64_t       result    = 0;
    std::uint64_t       delivered = 0;
    std::uint32_t       dones     = 0;

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        if (msg.tag == kJob) {
            Worker &w = workers[msg.dst];
            w.acc += job_value(msg.dst, msg.a, work);
            if (++w.received == messages)
                m.send(worker, qvobase::Msg{master, kDone, w.acc, w.received});
        } else {
            result += msg.a;
            delivered += msg.b + 1;
            if (++dones == actors) {
                watch.stop();
                m.stop();
            }
        }
    });
    mesh.start();

    watch.start();
    for (std::uint64_t i = 0; i < messages; ++i)
        for (std::uint32_t w = 0; w < actors; ++w)
            mesh.send(0, qvobase::Msg{w, kJob, i, 0});
    mesh.run();

    return qvo::Answer{result, delivered};
}

}  // namespace savina_fork_join_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join::params();
    spec.expected          = qvospec::savina::fork_join::expected;
    spec.expected_messages = qvospec::savina::fork_join::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; worker w is owned by thread w % cores, the master is the "
                             "calling thread; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Workers are slots of a vector, a job is one ring push and one "
        "ring pop, and nothing balances load: the floor for a fire-and-forget dispatch, not for "
        "scheduling 60 runnable actors on 2 threads",
        "worker w lives on thread w % cores, so with cores=2 half the jobs stay on the master's "
        "own thread (drained inline when its ring fills) and half cross a core",
        "cores=1 is one thread pushing every job into its own 65536-slot ring and draining it "
        "inline whenever it is full -- the floor for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_baseline::body);
}
