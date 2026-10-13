// @benchmark     savina/sieve
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h, with the
//                fork-join-create floor's notion of an actor created inside the window: a heap
//                node, allocated by the worker that owns it when its first message -- Create --
//                arrives.
// @idiom-note    The same placements qb gets (sieve::core_of_filter): filter f's id is
//                `f * W + worker`, so its owner is that worker. Creating the next filter is one
//                Create message to the new id, which the creator knows at once (it chooses it), and
//                the candidates follow it on the same ring -- no round trip, which is the least
//                creation on another worker can cost. A filter keeps its candidates' order because
//                the ring between two workers is FIFO and a worker runs one handler at a time
//                (the shared Mesh dispatch guard).

#include <qvospec/savina/sieve.h>

#include "../baseline_support.h"

#include <memory>
#include <vector>

namespace savina_sieve_baseline {

using namespace qvospec::savina::sieve;

// kCandidate: a = the number.  kCreate: a = the new filter's first prime.
enum Tag : std::uint32_t { kStart = 1, kCandidate, kCreate, kExit };

// A filter's node, allocated by its owner on Create: its decision, its successor's id.
struct alignas(kTallyLine) Node {
    Filter        filter;
    std::uint32_t next{0};
};

struct alignas(kTallyLine) ProducerState {
    std::uint64_t starts{0};
};


// At place=1 the filters alternate between the workers, filter f on worker (f + 1) % W, so the
// rings between workers form one cycle, 0 -> 1 -> ... -> W - 1 -> 0, and Mesh::send spins on a
// peer's full ring: if EVERY ring of the cycle filled at once, every worker would wait on the next
// forever. A ring that never holds its capacity can never fill, and then the cycle cannot close.
// So the run is refused only if every ring of the cycle is asked, over the whole run, for at least
// its capacity -- counted from the reference: the producer's candidates and Exit into filter 0's
// worker, and every filter's passed candidates, the Create and the Exit into its successor's.
bool can_deadlock(const qvo::Params &p, unsigned W, std::size_t capacity) {
    const Reference            r = reference(p);
    std::vector<std::uint64_t> load(W, 0);  // load[w]: the ring from worker w to worker (w + 1) % W
    const auto                 worker = [&](std::uint64_t f) {
        return static_cast<unsigned>(core_of_filter(f, 1, static_cast<int>(W)));
    };
    load[0] += candidates(p) + 1;  // the producer, on worker 0, to filter 0, on worker 1
    for (std::size_t f = 0; f + 1 < r.tallies.size(); ++f) load[worker(f)] += r.tallies[f].forwarded + 2;
    for (const std::uint64_t l : load)
        if (l < capacity) return false;
    return true;
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     limit = static_cast<std::uint64_t>(p.get("limit"));
    const auto     local = static_cast<std::uint64_t>(p.get("local"));
    const auto     place = p.get("place");
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;
    const auto     nfilt = static_cast<std::size_t>(filters(p));

    const auto id_of = [&](std::uint64_t f) {
        return static_cast<std::uint32_t>(f * W
                                          + static_cast<unsigned>(core_of_filter(f, place, static_cast<int>(W))));
    };
    const std::uint32_t producer = 0;  // id 0: worker 0

    std::vector<Tally>                      tallies(nfilt);
    std::vector<std::vector<std::uint64_t>> primes(nfilt);
    std::vector<std::unique_ptr<Node>>      nodes(nfilt);  // nodes[f], touched by f's owner only
    ProducerState                           prod;
    nodes[0]         = std::make_unique<Node>();
    nodes[0]->filter = Filter(2, local, tallies[0], primes[0]);

    qvobase::Mesh     mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        if (msg.tag == kStart) {  // Savina's NumberProducerActor: every candidate, then the Exit
            ++prod.starts;
            const std::uint32_t first = id_of(0);
            for (std::uint64_t n = 3; n < limit; n += 2) m.send(worker, qvobase::Msg{first, kCandidate, n, 0});
            m.send(worker, qvobase::Msg{first, kExit, 0, 0});
            return;
        }
        const std::uint64_t f = msg.dst / W;
        if (msg.tag == kCreate) {  // the first message a new filter receives: its node
            nodes[f]         = std::make_unique<Node>();
            nodes[f]->filter = Filter(msg.a, local, tallies[f], primes[f]);
            return;
        }
        Node &node = *nodes[f];
        if (msg.tag == kExit) {  // the last filter's ends the run
            ++tallies[f].exits;
            if (node.filter.linked()) m.send(worker, qvobase::Msg{node.next, kExit, 0, 0});
            else {
                watch.stop();
                m.stop();
            }
            return;
        }
        switch (node.filter.take(msg.a)) {
        case Filter::Verdict::pass: m.send(worker, qvobase::Msg{node.next, kCandidate, msg.a, 0}); break;
        case Filter::Verdict::grow:
            if (f + 1 >= nfilt) too_many_filters();
            node.next = id_of(f + 1);
            m.send(worker, qvobase::Msg{node.next, kCreate, msg.a, 0});
            break;
        default: break;  // dropped, or kept here
        }
    });
    if (place == 1 && W > 1 && can_deadlock(p, W, decltype(mesh)::kRingCapacity))
        qvo::not_applicable("savina/sieve floor, place=1: the filters alternate between the workers "
                            "and every ring between them would have to hold more messages than its "
                            "capacity over the run; workers each spinning on a full ring to the next "
                            "would never return (baseline_support.h, Mesh::send) -- the floor "
                            "refuses the run rather than risk a hang");
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{producer, kStart, 0, 0});
    mesh.run();

    // run() joined every worker: every tally is final, and reduced here, outside the window.
    return qvo::Answer{checksum(tallies, primes), delivered(tallies, prod.starts)};
}

}  // namespace savina_sieve_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sieve::params();
    // The placement of this cell: qb's (benchmarks/savina/sieve.md), the floor bounding qb.
    spec.params["place"]   = 0;
    spec.expected          = qvospec::savina::sieve::expected;
    spec.expected_messages = qvospec::savina::sieve::expected_messages;
    spec.work_unit         = qvospec::savina::sieve::kWorkUnit;
    spec.work_units        = qvospec::savina::sieve::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) pair; "
                             "a filter is a heap node its owner allocates on Create, placed as qb "
                             "places it; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are integers, a filter is created by one Create message "
        "to the id its creator chose, and a hand-off is one ring push and one ring pop: the floor "
        "for a growing chain, not for finding the next actor",
        "the producer on worker 0, filter f where qb puts it (sieve::core_of_filter): every filter "
        "on worker 1 % cores with place=0, filter f on worker (f + 1) % cores with place=1. The "
        "run's `place` is in its params",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_sieve_baseline::body);
}
