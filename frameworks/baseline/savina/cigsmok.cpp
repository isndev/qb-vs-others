// @benchmark     savina/cigsmok
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the arbiter
//                is actor 0 and smoker j is actor j + 1, so the mesh's own routing (actor d on
//                worker d % W) puts the arbiter on worker 0 and smoker j on worker (j + 1) % W.
// @idiom-note    The floor does what the semantics require and nothing else: a round is one ring
//                push to the chosen smoker and one back, and the smoker pushes its acknowledgement
//                BEFORE it smokes, so the acknowledgement is in the arbiter's ring while the smoke
//                runs -- the reference's order. Every smoker's state is its own cache line, touched
//                only by the worker that owns it. With cores=2 the placement is qb's: the arbiter
//                on worker 0, half the smokers beside it and half on worker 1.

#include <qvospec/savina/cigsmok.h>

#include "../baseline_support.h"

#include <cstdio>
#include <vector>

namespace savina_cigsmok_baseline {

using namespace qvospec::savina::cigsmok;

// kSmoke: a = round, b = period (to actor smoker + 1). kStarted: a = round, b = the smoker's
// number. kReport: a = partial checksum, b = messages. kStart and kExit carry nothing.
enum Tag : std::uint32_t { kStart = 1, kSmoke, kStarted, kExit, kReport };

// One smoker's state, on its own cache line: smokers owned by different workers sit side by side
// in the vector, and a shared line would be a cost no framework's separately allocated actor pays.
struct alignas(qvobase::kCacheLine) SmokerState {
    std::uint64_t acc{0};  // the smoker's terms of the checksum (cigsmok.h)
    std::uint64_t received{0};
};

struct alignas(qvobase::kCacheLine) ArbiterState {
    std::uint64_t outstanding{0};  // the round whose StartedSmoking is awaited
    std::uint64_t played{0};       // rounds acknowledged
    bool          exiting{false};
    std::uint64_t reports{0};
    std::uint64_t acc{0};  // the arbiter's terms of the checksum (cigsmok.h)
    std::uint64_t messages{0};  // reported by the smokers
    std::uint64_t received{0};
    std::uint64_t stale{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     rounds  = at_least_one(p.get("rounds"), "rounds");
    const auto     smokers = at_least_one(p.get("smokers"), "smokers");
    const auto     smoke   = at_least_one(p.get("smoke"), "smoke");
    const auto     cores   = static_cast<unsigned>(p.get("cores"));
    const bool     spin    = p.get("wait") != 0;
    const unsigned W       = cores < 1 ? 1u : cores;

    constexpr std::uint32_t kArbiter = 0;  // smoker j is actor j + 1

    std::vector<SmokerState> states(static_cast<std::size_t>(smokers));
    ArbiterState             a;

    std::uint64_t checksum = 0;
    std::uint64_t messages = 0;

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        // Put the ingredients on the table for round `round`: the draw names the smoker and the
        // period.
        auto choose = [&](std::uint64_t round) {
            const std::uint32_t smoker = smoker_of(round, smokers);
            a.outstanding              = round;
            m.send(worker, qvobase::Msg{smoker + 1, kSmoke, round, period_of(round, smoke)});
        };

        switch (msg.tag) {
        case kStart: {
            ++a.received;
            choose(0);
            break;
        }
        case kSmoke: {  // StartSmoking: acknowledge first, then smoke, as the reference does
            const std::uint32_t number = msg.dst - 1;
            SmokerState        &s      = states[number];
            ++s.received;
            m.send(worker, qvobase::Msg{kArbiter, kStarted, msg.a, number});
            s.acc += smoke_term(number, msg.a, static_cast<std::uint32_t>(msg.b));
            break;
        }
        case kStarted: {
            // Every StartedSmoking is folded into the sum; only the one naming the outstanding
            // round plays the next -- a duplicate completes the run with a wrong sum.
            ++a.received;
            a.acc += ack_term(static_cast<std::uint32_t>(msg.b), msg.a);
            if (a.exiting || msg.a != a.outstanding) {
                ++a.stale;
                break;
            }
            if (++a.played < rounds) {
                choose(a.played);
                break;
            }
            a.exiting = true;
            for (std::uint64_t j = 0; j < smokers; ++j)
                m.send(worker, qvobase::Msg{static_cast<std::uint32_t>(j + 1), kExit, 0, 0});
            break;
        }
        case kExit: {
            const std::uint32_t number = msg.dst - 1;
            SmokerState        &s      = states[number];
            ++s.received;
            s.acc += exit_term(number);
            m.send(worker, qvobase::Msg{kArbiter, kReport, s.acc, s.received});
            break;
        }
        case kReport: {
            ++a.received;
            a.acc += msg.a;
            a.messages += msg.b;
            if (++a.reports != smokers) break;
            checksum = a.acc;
            messages = a.messages + a.received;
            watch.stop();
            m.stop();
            break;
        }
        }
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{kArbiter, kStart, 0, 0});
    mesh.run();

    if (a.stale != 0)
        std::fprintf(stderr,
                     "savina/cigsmok baseline: %llu StartedSmoking named no outstanding round\n",
                     static_cast<unsigned long long>(a.stale));
    return qvo::Answer{checksum, messages};
}

}  // namespace savina_cigsmok_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::cigsmok::params();
    spec.expected          = qvospec::savina::cigsmok::expected;
    spec.expected_messages = qvospec::savina::cigsmok::expected_messages;
    spec.work_unit         = qvospec::savina::cigsmok::kWorkUnit;
    spec.work_units        = qvospec::savina::cigsmok::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the arbiter is actor 0 on worker 0, smoker j actor j + 1 on "
                             "worker (j + 1) % cores; the acknowledgement is pushed before the "
                             "smoke; not an actor framework";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A round is one ring push to the chosen smoker and one back, a "
        "smoker's state one cache line: the floor for what this coordination costs",
        "the arbiter is on worker 0 and smoker j on worker (j + 1) % cores -- the static placement "
        "qb's cell has, so this floor bounds the placing frameworks and NOT the pools",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_cigsmok_baseline::body);
}
