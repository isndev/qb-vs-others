// @benchmark     savina/philosophers
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the
//                arbitrator is actor 0 on worker 0 and philosopher i is owned by worker
//                1 + i % (cores - 1) -- qb's placement: every philosopher on the far worker when
//                there are two, spread over the others when there are more, all on 0 when one.
// @idiom-note    A request is one ring push from the philosopher's worker to the arbitrator's,
//                an answer one push back, a retry or a release one push again, the Start one push
//                to the philosopher's own worker. What the floor lacks, and the frameworks pay
//                for, is the arbitrator's MAILBOX: at cores=2 the 20 philosophers share ONE SPSC
//                ring into worker 0, so the 20-writer fan-in collapses to a single producer -- the
//                contention engineered out by placement. It bounds the ONE-core cell only: at
//                cores=2 every message is its own ring hand-off, where qb batches a pass's events
//                into one flush and runs under this floor.

#include <qvospec/savina/philosophers.h>

#include "../baseline_support.h"

#include <limits>
#include <vector>

namespace savina_philosophers_baseline {

using namespace qvospec::savina::philosophers;

// kStartTag: none.  kHungryTag / kDoneTag: a = philosopher.  kEatTag / kDeniedTag: none.
// kExitTag: a = philosopher << 40 | messages, b = the philosopher's fold.
enum Tag : std::uint32_t {
    kStartTag  = 1,
    kHungryTag = 2,
    kEatTag    = 3,
    kDeniedTag = 4,
    kDoneTag   = 5,
    kExitTag   = 6
};

struct Philosopher {
    std::uint64_t starts{0};
    std::uint64_t eats{0};
    std::uint64_t chk{0};
    std::uint64_t received{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     n      = static_cast<std::uint32_t>(p.get("philosophers"));
    const auto     rounds = static_cast<std::uint64_t>(p.get("rounds"));
    const auto     cores  = static_cast<unsigned>(p.get("cores"));
    const bool     spin   = p.get("wait") != 0;
    const unsigned W      = cores < 1 ? 1u : cores;

    constexpr std::uint32_t kFree      = std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t     arbitrator = 0;
    // Actor ids are slot * W + owner (baseline_support.h: the owner is id % W). The arbitrator is
    // id 0; philosopher i takes slot i + 1 on worker 1 + i % (W - 1), qb's placement.
    auto worker_of = [W](std::uint32_t i) { return W == 1 ? 0u : 1u + i % (W - 1); };
    auto actor_of  = [W, worker_of](std::uint32_t i) { return (i + 1) * W + worker_of(i); };
    auto index_of  = [W](std::uint32_t actor) { return actor / W - 1; };

    std::vector<Philosopher>   state(n);
    std::vector<std::uint32_t> owner(n, kFree);
    std::vector<std::uint64_t> grants(n, 0);
    std::vector<std::uint64_t> dones(n, 0);
    std::uint64_t              chk       = 0;
    std::uint64_t              received  = 0;
    std::uint64_t              refused   = 0;  // observed, not asserted: the scheduler's number
    std::uint32_t              exited    = 0;
    bool                       violation = false;

    // Nothing of a philosopher's or the arbitrator's state is touched after a send: a send to a
    // FULL ring owned by the sending worker drains inline (baseline_support.h). With at most two
    // messages in flight per philosopher it never fills here, and the code does not rely on it.
    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kStartTag: {
            const std::uint32_t i  = index_of(msg.dst);
            Philosopher        &ph = state[i];
            ++ph.received;
            ph.chk += term(kStart, i, ++ph.starts);
            m.send(worker, qvobase::Msg{arbitrator, kHungryTag, i, 0});
            break;
        }
        case kDeniedTag:
            m.send(worker, qvobase::Msg{arbitrator, kHungryTag, index_of(msg.dst), 0});
            break;
        case kEatTag: {
            const std::uint32_t i  = index_of(msg.dst);
            Philosopher        &ph = state[i];
            ++ph.received;
            ph.chk += term(kEat, i, ++ph.eats);
            const bool last = ph.eats == rounds;
            const auto fold = ph.chk;
            const auto msgs = ph.received;
            m.send(worker, qvobase::Msg{arbitrator, kDoneTag, i, 0});
            if (!last)
                m.send(worker, qvobase::Msg{msg.dst, kStartTag, 0, 0});
            else
                m.send(worker,
                       qvobase::Msg{arbitrator, kExitTag, (std::uint64_t{i} << 40) | msgs, fold});
            break;
        }
        case kHungryTag: {
            const auto          i     = static_cast<std::uint32_t>(msg.a);
            const std::uint32_t left  = i;
            const std::uint32_t right = (i + 1) % n;
            if (owner[left] != kFree || owner[right] != kFree) {
                ++refused;
                m.send(worker, qvobase::Msg{actor_of(i), kDeniedTag, 0, 0});
                break;
            }
            owner[left] = owner[right] = i;
            ++received;
            chk += term(kGrant, i, ++grants[i]);
            m.send(worker, qvobase::Msg{actor_of(i), kEatTag, 0, 0});
            break;
        }
        case kDoneTag: {
            const auto          i     = static_cast<std::uint32_t>(msg.a);
            const std::uint32_t left  = i;
            const std::uint32_t right = (i + 1) % n;
            ++received;
            chk += term(kDone, i, ++dones[i]);
            if (owner[left] != i || owner[right] != i) violation = true;
            owner[left] = owner[right] = kFree;
            break;
        }
        case kExitTag: {
            const auto i = static_cast<std::uint32_t>(msg.a >> 40);
            ++received;
            chk += term(kExit, i, dones[i]) + msg.b;
            received += msg.a & ((std::uint64_t{1} << 40) - 1);
            if (++exited == n) {
                watch.stop();
                m.stop();
            }
            break;
        }
        }
    });
    mesh.start();

    watch.start();
    for (std::uint32_t i = 0; i < n; ++i)
        mesh.send(0, qvobase::Msg{actor_of(i), kStartTag, 0, 0});
    mesh.run();

    qvo::Answer answer{chk + (violation ? kForkViolation : 0), received};
    answer.observed[kObservedRefused] = refused;
    return answer;
}

}  // namespace savina_philosophers_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::philosophers::params();
    spec.expected          = qvospec::savina::philosophers::expected;
    spec.expected_messages = qvospec::savina::philosophers::expected_messages;
    spec.work_unit         = qvospec::savina::philosophers::kWorkUnit;
    spec.work_units        = qvospec::savina::philosophers::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the arbitrator is owned by thread 0 and philosopher i by "
                             "thread 1 + i % (cores - 1); not an actor framework and not ranked "
                             "as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Philosophers are slots of a vector and the arbitrator has no "
        "mailbox: the 20 philosophers share one SPSC ring into worker 0, so the 20-producer "
        "fan-in the frameworks pay for is engineered out here by placement",
        "the arbitrator lives on thread 0 and philosopher i on thread 1 + i % (cores - 1), so "
        "with cores=2 every request and every answer crosses a core -- the same placement qb's "
        "cell fixes",
        "cores=1 is one thread with the arbitrator and the philosophers in its own ring -- the "
        "floor for single-threaded dispatch, still a real queue, and the cell this floor bounds",
        "with cores=2 the floor is NOT a bound: every message is its own ring hand-off to the "
        "other thread, where qb publishes a pass's events in one batched flush and runs under "
        "this floor -- read it there as the per-message cost of a hand-off",
        "how many requests are refused depends on the interleaving: it is REPORTED beside the "
        "cell (observed `refused`, Savina's 'Num retries'), never asserted; every refused "
        "request and its retry are delivered and timed",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "with 20 philosophers retrying is rare"};

    return qvo::run(argc, argv, std::move(spec), savina_philosophers_baseline::body);
}
