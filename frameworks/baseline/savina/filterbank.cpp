// @benchmark     savina/filterbank
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: every actor
//                of the reference an integer, its state the spec's filters on a line of their own,
//                every value one ring push and one ring pop.
// @idiom-note    The same static placement qb gets (filterbank::core_of): the producer, the source
//                and the branches on worker 0, bank j on worker j % cores, the integrator, the
//                combine and the sink on the last worker; an actor's id is `number * W + worker`
//                so that its owner is that worker. A round reaches the combine as its index into a
//                table the integrator fills -- the two share a worker, and a pointer is the least a
//                message carrying `channels` values can be. A worker runs one handler at a time
//                (the shared Mesh dispatch guard), so a filter's outputs keep its inputs' order.

#include <qvospec/savina/filterbank.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_filterbank_baseline {

using namespace qvospec::savina::filterbank;

// kValue: a = the value's bits.  kTagged: a = bits, b = channel.  kRound: a = the round's index.
enum Tag : std::uint32_t { kNext = 1, kBoot, kValue, kTagged, kRound, kExit };

// A bank's filters: all seven of its actors live on one worker, so the bank's state is one block,
// on lines no other worker writes.
struct alignas(kTallyLine) BankState {
    Delay   delay1;
    Fir     fir1;
    Sampler sampler;
    Delay   delay2;
    Fir     fir2;
};

struct alignas(kTallyLine) HeadState {  // the producer and the source, worker 0
    std::uint64_t sent{0};
    std::uint64_t current{0};
};

struct alignas(kTallyLine) TailState {  // the integrator and the combine, the last worker
    Join                join;
    std::vector<double> table;  // round r is table[r * channels, (r + 1) * channels)
    std::uint64_t       next_round{0};
};


inline double as_double(std::uint64_t bits) noexcept { return std::bit_cast<double>(bits); }
inline std::uint64_t as_bits(double value) noexcept { return std::bit_cast<std::uint64_t>(value); }

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     simulations = static_cast<std::uint64_t>(p.get("simulations"));
    const auto     columns     = static_cast<std::uint32_t>(p.get("columns"));
    const auto     channels    = static_cast<std::uint32_t>(p.get("channels"));
    const auto     cores       = static_cast<unsigned>(p.get("cores"));
    const bool     spin        = p.get("wait") != 0;
    const auto     nrounds     = rounds(p);
    const unsigned W           = cores < 1 ? 1u : cores;

    const Coefficients     coefficients(channels, columns);
    std::vector<Tally>     tallies(actors(channels));
    std::vector<BankState> banks(channels);
    for (std::uint32_t j = 0; j < channels; ++j) {
        banks[j].delay1  = Delay(columns - 1);
        banks[j].fir1    = Fir(columns, coefficients.h(j));
        banks[j].sampler = Sampler(columns);
        banks[j].delay2  = Delay(columns - 1);
        banks[j].fir2    = Fir(columns, coefficients.f(j));
    }
    HeadState head;
    TailState tail;
    tail.join  = Join(channels, nrounds);
    tail.table.resize(static_cast<std::size_t>(nrounds) * channels);
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(nrounds));
    SequenceSlot               out{Sequence(sequence.data(), nrounds)};

    const auto id_of = [&](std::uint32_t actor) {
        return actor * W + static_cast<unsigned>(core_of(actor, channels, static_cast<int>(W)));
    };
    const std::uint32_t producer = id_of(kProducer), source = id_of(kSource),
                        branches = id_of(kBranches), integ = id_of(integrator(channels)),
                        comb = id_of(combiner(channels)), snk = id_of(sink(channels));

    qvobase::Mesh     mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        const std::uint32_t actor = msg.dst / W;
        Tally              &t     = tallies[actor];
        const auto          send  = [&](std::uint32_t to, std::uint32_t tag, std::uint64_t a = 0,
                                    std::uint64_t b = 0) {
            m.send(worker, qvobase::Msg{to, tag, a, b});
        };
        if (msg.tag == kExit) {
            ++t.exits;
            if (actor == kSource) send(branches, kExit);
            else if (actor == kBranches)
                for (std::uint32_t j = 0; j < channels; ++j) send(id_of(bank_actor(j, kBank)), kExit);
            else if (actor == integrator(channels)) {
                if (t.exits == channels) send(comb, kExit);
            } else if (actor == combiner(channels)) send(snk, kExit);
            else if (actor == sink(channels)) {  // the Exit follows the last sum: it ends the run
                watch.stop();
                m.stop();
            } else {  // a bank's actor: the next of the bank, or the integrator after the tagger
                const std::uint32_t role = (actor - 3) % kRoles;
                send(role == kTagger ? integ : id_of(actor + 1), kExit);
            }
            return;
        }
        switch (msg.tag) {
        case kNext:  // the producer
            ++t.count;
            if (head.sent == simulations) send(source, kExit);
            else {
                ++head.sent;
                send(source, kBoot);
            }
            return;
        case kBoot:  // the source
            ++t.count;
            send(branches, kValue, as_bits(sample(head.current++)));
            send(producer, kNext);
            return;
        case kTagged: {  // the integrator
            const double v = as_double(msg.a);
            t.add(v);
            if (!tail.join.put(static_cast<std::uint32_t>(msg.b), v)) return;
            bool more = true;
            while (more) {
                double *round = &tail.table[static_cast<std::size_t>(tail.next_round) * channels];
                more          = tail.join.take(round);
                send(comb, kRound, tail.next_round++);
            }
            return;
        }
        case kRound: {  // the combine
            const double *round = &tail.table[static_cast<std::size_t>(msg.a) * channels];
            t.add_round(round, channels);
            send(snk, kValue, as_bits(combine(round, channels)));
            return;
        }
        default:  // kValue
            break;
        }
        const double v = as_double(msg.a);
        t.add(v);
        if (actor == kBranches) {
            for (std::uint32_t j = 0; j < channels; ++j) send(id_of(bank_actor(j, kBank)), kValue, msg.a);
            return;
        }
        if (actor == sink(channels)) {
            out.sequence.take(v);
            return;
        }
        const std::uint32_t j    = (actor - 3) / kRoles;
        const std::uint32_t role = (actor - 3) % kRoles;
        BankState          &bank = banks[j];
        const std::uint32_t next = id_of(actor + 1);
        double              y    = 0;
        switch (role) {
        case kBank: send(next, kValue, msg.a); break;
        case kDelay1: send(next, kValue, as_bits(bank.delay1.step(v))); break;
        case kFir1:
            if (bank.fir1.step(v, y)) send(next, kValue, as_bits(y));
            break;
        case kSampler: send(next, kValue, as_bits(bank.sampler.step(v))); break;
        case kDelay2: send(next, kValue, as_bits(bank.delay2.step(v))); break;
        case kFir2:
            if (bank.fir2.step(v, y)) send(next, kValue, as_bits(y));
            break;
        default: send(integ, kTagged, msg.a, j); break;  // the tagger
        }
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{producer, kNext, 0, 0});  // the reference's first Next; worker 0's
    mesh.run();

    // run() joined every worker: every tally is final, and reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence), delivered(tallies)};
}

}  // namespace savina_filterbank_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::filterbank::params();
    spec.expected          = qvospec::savina::filterbank::expected;
    spec.expected_messages = qvospec::savina::filterbank::expected_messages;
    spec.work_unit         = qvospec::savina::filterbank::kWorkUnit;
    spec.work_units        = qvospec::savina::filterbank::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) pair; "
                             "qb's placement (head on worker 0, bank j on j % cores, the join on "
                             "the last); not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are integers, a hand-off is one ring push and one ring pop, "
        "and there is no scheduler: the floor for the bank's filters and its fan-out and fan-in, "
        "not for finding the next actor",
        "the producer, the source and the branches on worker 0, bank j on worker j % cores, the "
        "integrator, the combine and the sink on the last worker -- the placement qb gets",
        "a round reaches the combine as an index into a table the integrator fills (the two share "
        "a worker): the least a message carrying `channels` values can be",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_filterbank_baseline::body);
}
