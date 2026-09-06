// @benchmark     savina/chameneos
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the mall
//                is actor 0 on worker 0, creature c is actor 1 + c * cores, so every creature is
//                owned by worker 1 % cores -- the far worker when there are two.
// @idiom-note    A request is one ring push from the creature's worker to the mall's, an
//                announcement one push back. What the floor lacks, and the frameworks pay for,
//                is the mall's MAILBOX: here the 100 creatures share ONE SPSC ring into worker
//                0, so the 100-writer fan-in the spec describes collapses to a single producer.
//                That is the point of the floor -- it is what the workload costs when the
//                contention is engineered out by placement.

#include <qvospec/savina/chameneos.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_chameneos_baseline {

using namespace qvospec::savina::chameneos;

// kStart: none.  kMeet: a = creature index, b = colour.  kMeeting: a = k, b = other colour.
// kExit: none.  kCount: a = meetings << 32 | messages, b = acc.
enum Tag : std::uint32_t { kStart = 1, kMeet = 2, kMeeting = 3, kExit = 4, kCount = 5 };

struct Creature {
    Colour        colour{kYellow};
    std::uint64_t meetings{0};
    std::uint64_t acc{0};
    std::uint64_t received{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     creatures = static_cast<std::uint32_t>(p.get("chameneos"));
    const auto     meetings  = static_cast<std::uint32_t>(p.get("meetings"));
    const auto     cores     = static_cast<unsigned>(p.get("cores"));
    const bool     spin      = p.get("wait") != 0;
    const unsigned W         = cores < 1 ? 1u : cores;

    const std::uint32_t mall = 0;
    auto                actor_of = [W](std::uint32_t c) { return 1 + c * W; };
    auto                index_of = [W](std::uint32_t actor) { return (actor - 1) / W; };

    std::vector<Creature> state(creatures);
    for (std::uint32_t c = 0; c < creatures; ++c) state[c].colour = initial_colour(c);

    std::uint32_t k              = 0;
    bool          waiting        = false;
    std::uint32_t waiting_index  = 0;
    Colour        waiting_colour = kYellow;
    std::uint64_t mall_received  = 0;
    std::uint64_t total_meetings = 0;
    std::uint32_t counted        = 0;
    std::uint64_t result         = 0;
    std::uint64_t delivered      = 0;

    auto request = [&](auto &m, unsigned worker, std::uint32_t index) {
        m.send(worker, qvobase::Msg{mall, kMeet, index, state[index].colour});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kStart: {
            const std::uint32_t index = index_of(msg.dst);
            ++state[index].received;
            request(m, worker, index);
            break;
        }
        case kMeet: {
            ++mall_received;
            const auto index  = static_cast<std::uint32_t>(msg.a);
            const auto colour = static_cast<Colour>(msg.b);
            if (k == meetings) {
                m.send(worker, qvobase::Msg{actor_of(index), kExit, 0, 0});
                break;
            }
            if (!waiting) {
                waiting        = true;
                waiting_index  = index;
                waiting_colour = colour;
                break;
            }
            waiting = false;
            m.send(worker, qvobase::Msg{actor_of(index), kMeeting, k, waiting_colour});
            m.send(worker, qvobase::Msg{actor_of(waiting_index), kMeeting, k, colour});
            ++k;
            break;
        }
        case kMeeting: {
            const std::uint32_t index = index_of(msg.dst);
            Creature           &c     = state[index];
            ++c.received;
            c.colour = complement(c.colour, static_cast<Colour>(msg.b));
            c.acc += qvo::mix(msg.a);
            ++c.meetings;
            request(m, worker, index);
            break;
        }
        case kExit: {
            Creature &c = state[index_of(msg.dst)];
            ++c.received;
            m.send(worker, qvobase::Msg{mall, kCount, (c.meetings << 32) | c.received, c.acc});
            break;
        }
        case kCount:
            ++mall_received;
            result += msg.b;
            delivered += msg.a & 0xffffffffu;
            total_meetings += msg.a >> 32;
            if (++counted == creatures) {
                result += qvo::mix(total_meetings);
                delivered += mall_received;
                watch.stop();
                m.stop();
            }
            break;
        }
    });
    mesh.start();

    watch.start();
    for (std::uint32_t c = 0; c < creatures; ++c)
        mesh.send(0, qvobase::Msg{actor_of(c), kStart, 0, 0});
    mesh.run();

    return qvo::Answer{result, delivered};
}

}  // namespace savina_chameneos_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::chameneos::params();
    spec.expected          = qvospec::savina::chameneos::expected;
    spec.expected_messages = qvospec::savina::chameneos::expected_messages;
    spec.work_unit         = qvospec::savina::chameneos::kWorkUnit;
    spec.work_units        = qvospec::savina::chameneos::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the mall is owned by thread 0 and every creature by thread "
                             "1 % cores; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Creatures are slots of a vector and the mall has no mailbox: "
        "the 100 creatures share one SPSC ring into worker 0, so the 100-producer fan-in the "
        "frameworks pay for is engineered out here by placement",
        "the mall lives on thread 0 and every creature on thread 1 % cores, so with cores=2 "
        "every request and every announcement crosses a core -- the same placement qb's cell "
        "fixes",
        "cores=1 is one thread with the mall and the creatures in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "with 100 creatures in flight is rare"};

    return qvo::run(argc, argv, std::move(spec), savina_chameneos_baseline::body);
}
