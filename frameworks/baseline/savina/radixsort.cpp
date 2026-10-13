// @benchmark     savina/radixsort
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the source,
//                the stages and the validation are integers whose state is a slot of a vector,
//                and a value moving down the chain is one ring push and one ring pop.
// @idiom-note    The same static placement qb gets: the actors in pipeline order cut into `cores`
//                contiguous blocks (radixsort::core_of), an actor's id `position * W + worker` so
//                that its owner is that block's worker. Every per-sender FIFO the sort needs is
//                the ring's own, and a worker runs one handler at a time (the shared Mesh dispatch guard): the source
//                and a flushing stage fill the worker's own ring, which Mesh::send drains inline.

#include <qvospec/savina/radixsort.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_radixsort_baseline {

using namespace qvospec::savina::radixsort;

// Msg.a = the value.
enum Tag : std::uint32_t { kValue = 1, kStart = 2 };

// A stage's state beside its decision, on a line of its own: stages of two workers are never
// adjacent in memory. Its Counts are its tally slot's, written by the worker that owns the stage.
struct alignas(kTallyLine) StageState {
    Stage                      stage;
    std::uint32_t              next{0};  // the id of the actor after it in the chain
    std::vector<std::uint64_t> held;
};

struct alignas(kTallyLine) SourceState {
    std::uint64_t starts{0};
};


qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     values = static_cast<std::uint64_t>(p.get("values"));
    const auto     bits   = static_cast<std::uint32_t>(p.get("bits"));
    const long long seed  = p.get("seed");
    const auto     cores  = static_cast<unsigned>(p.get("cores"));
    const bool     spin   = p.get("wait") != 0;
    const unsigned W      = cores < 1 ? 1u : cores;

    const std::uint32_t npos  = positions(bits);
    const auto          id_of = [&](std::uint32_t position) {
        return position * W + core_of(position, npos, static_cast<int>(W));
    };
    const std::uint32_t source     = id_of(0);
    const std::uint32_t validation = id_of(bits + 1);

    std::vector<Tally>          tallies(bits);
    std::vector<StageState>     stages(bits);
    std::vector<std::uint64_t>  sequence(static_cast<std::size_t>(values));
    ValidationSlot              validator{Validation(sequence.data(), values)};
    SourceState                 src;
    for (std::uint32_t k = 0; k < bits; ++k) {
        stages[k].stage = Stage(k, values, tallies[k]);
        stages[k].next  = id_of(k + 2);
        stages[k].held.reserve(static_cast<std::size_t>(values));
    }

    qvobase::Mesh     mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        const std::uint32_t position = msg.dst / W;
        if (msg.tag == kStart) {  // Savina's IntSourceActor: every value from this one handler
            ++src.starts;
            Source              draw(seed, bits);
            const std::uint32_t first = id_of(1);
            for (std::uint64_t i = 0; i < values; ++i)
                m.send(worker, qvobase::Msg{first, kValue, draw.next(), 0});
        } else if (position == bits + 1) {  // the last value ends the run; one after it fails it
            if (validator.validation.take(msg.a)) {
                watch.stop();
                m.stop();
            }
        } else {
            StageState &s = stages[position - 1];
            if (s.stage.take(msg.a)) m.send(worker, qvobase::Msg{s.next, kValue, msg.a, 0});
            else s.held.push_back(msg.a);
            if (s.stage.complete())
                for (const std::uint64_t h : s.held) m.send(worker, qvobase::Msg{s.next, kValue, h, 0});
        }
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{source, kStart, 0, 0});  // the source is worker 0's: position 0
    mesh.run();

    // run() joined every worker: every tally is final, and reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence),
                       delivered(tallies, validator.validation.count(), src.starts)};
}

}  // namespace savina_radixsort_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::radixsort::params();
    spec.expected          = qvospec::savina::radixsort::expected;
    spec.expected_messages = qvospec::savina::radixsort::expected_messages;
    spec.work_unit         = qvospec::savina::radixsort::kWorkUnit;
    spec.work_units        = qvospec::savina::radixsort::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) pair; "
                             "the chain cut into `cores` contiguous blocks, qb's placement; not an "
                             "actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are integers, a hand-off is one ring push and one ring pop, "
        "and there is no scheduler: the floor for a chain of stable partitions, not for finding the "
        "next actor",
        "the source, the stages and the validation, in pipeline order, are cut into `cores` "
        "contiguous blocks -- the placement qb gets -- so with cores=2 a value crosses a core once",
        "cores=1 is one thread pushing into its own ring and draining it inline when the source or a "
        "flushing stage fills it -- the floor for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_radixsort_baseline::body);
}
