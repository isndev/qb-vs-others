// @benchmark     savina/big
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: `actors`
//                integers owned by worker a % cores, all pinging one another.
// @idiom-note    Every actor is one slot of a vector holding its target sequence, its accumulator
//                and its ping count. A ping is one ring push and one ring pop, a pong the same.
//                What the floor lacks, and the frameworks pay for, is a per-actor MAILBOX:
//                here a worker's inbound ring is shared by every actor it owns, so the 120
//                producers per mailbox that the spec describes collapse to `cores` producers per
//                ring. That is the point of the floor -- it is what the workload costs when
//                contention is engineered out by static placement.

#include <qvospec/savina/big.h>

#include "../baseline_support.h"

#include <thread>
#include <vector>

namespace savina_big_baseline {

using namespace qvospec::savina::big;

// kStart: none.  kPing: a = pinger, b = k.  kPong: a = value.  kDone: a = acc, b = messages.
enum Tag : std::uint32_t { kStart = 1, kPing = 2, kPong = 3, kDone = 4 };

struct Actor {
    TargetSequence seq;
    std::uint64_t  acc{0};
    std::uint32_t  sent{0};
    // Messages proven delivered TO or BY this actor: its start, and two per pong -- the pong
    // itself and the ping it answers, which the pong is the proof of. Pings received from
    // peers are not counted here: they may keep arriving after this actor has reported done,
    // and a count that depends on that race is not a count (measured: 4 726 888 of 4 800 240).
    std::uint64_t  messages{0};

    Actor(std::uint32_t self, std::uint32_t n) : seq(self, n) {}
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto     pings  = static_cast<std::uint32_t>(p.get("pings"));
    const auto     cores  = static_cast<unsigned>(p.get("cores"));
    const bool     spin   = p.get("wait") != 0;
    const unsigned W      = cores < 1 ? 1u : cores;

    const std::uint32_t sink = actors;

    std::vector<Actor> state;
    state.reserve(actors);
    for (std::uint32_t a = 0; a < actors; ++a) state.emplace_back(a, actors);

    std::uint64_t result    = 0;
    std::uint64_t delivered = 0;
    std::uint32_t dones     = 0;

    auto ping = [&](auto &m, unsigned worker, std::uint32_t self, Actor &a) {
        const std::uint32_t k = a.sent++;
        m.send(worker, qvobase::Msg{a.seq.next(), kPing, self, k});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kStart: {
            Actor &a = state[msg.dst];
            ++a.messages;
            ping(m, worker, msg.dst, a);
            break;
        }
        case kPing: {
            const auto pinger = static_cast<std::uint32_t>(msg.a);
            const auto k      = static_cast<std::uint32_t>(msg.b);
            m.send(worker, qvobase::Msg{pinger, kPong, pong_value(pinger, msg.dst, k), 0});
            break;
        }
        case kPong: {
            Actor &a = state[msg.dst];
            a.messages += 2;
            a.acc += msg.a;
            if (a.sent < pings)
                ping(m, worker, msg.dst, a);
            else
                m.send(worker, qvobase::Msg{sink, kDone, a.acc, a.messages});
            break;
        }
        case kDone:
            result += msg.a;
            delivered += msg.b + 1;
            if (++dones == actors) {
                watch.stop();
                m.stop();
            }
            break;
        }
    });
    mesh.start();

    watch.start();
    for (std::uint32_t a = 0; a < actors; ++a) mesh.send(0, qvobase::Msg{a, kStart, 0, 0});
    mesh.run();

    return qvo::Answer{result, delivered};
}

}  // namespace savina_big_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::big::params();
    spec.expected          = qvospec::savina::big::expected;
    spec.expected_messages = qvospec::savina::big::expected_messages;
    spec.work_unit         = qvospec::savina::big::kWorkUnit;
    spec.work_units        = qvospec::savina::big::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; actor a is owned by thread a % cores; not an actor framework "
                             "and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are slots of a vector and there is no per-actor "
        "mailbox: a worker's inbound ring is shared by every actor it owns, so the 120-producer "
        "mailbox contention the frameworks pay for is engineered out here by static placement",
        "actor a lives on thread a % cores, so with cores=2 roughly half the pings and half the "
        "pongs cross a core",
        "cores=1 is one thread with every actor in its own ring -- the floor for single-threaded "
        "dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "with 120 actors in flight is rare"};

    return qvo::run(argc, argv, std::move(spec), savina_big_baseline::body);
}
