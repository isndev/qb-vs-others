// The negative control's filterbank subject: savina/filterbank's verification, watched rejecting
// each fault class the shape can suffer (Huly QB-246; the thread-ring subject's method, QB-1017).
//
// The whole actor graph -- producer, source, branches, `channels` banks of seven, integrator,
// combine, sink -- simulated on one thread: a FIFO of messages, every filter the spec's (Delay,
// Fir, Sampler, Join), every actor a Tally, reduced through the spec's checksum() and delivered()
// like every adapter. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none           correct -- the POSITIVE control: must verify.
//   duplicate      bank 0's first delay sends its 100th output twice.
//   lost           bank 1's sampler drops its 50th output.
//   misroute       bank 0's tagger names its 5th value channel 1.
//   untouched      bank 2's sampler keeps a value it should have zeroed (its 2nd).
//   routed-around  bank 0's second delay sends to the tagger: the second FIR never runs.
//   swapped        banks 0 and 1 filter with each other's H coefficients -- a CONSISTENT fault:
//                  every actor receives exactly as many values as it should, and only the sums
//                  and the sequence see it.
//   lost-exit      bank 2's tagger drops its Exit: the integrator waits for it, the run BLOCKS and
//                  the harness's deadline fails it.
//
// Each plant must be REJECTED. After a run the subject says on stderr whether every actor's count
// equals the closed form ("every count as the closed form says: yes"), which is how the battery
// shows the swapped plant is the case a count misses. Run with --param channels=1, the spec must
// refuse the run before anything is built (exit 2).
//
// Like the other subjects it is NOT built by qvo_add_benchmark and its name does not start with
// "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/filterbank.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qvospec::savina::filterbank;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

[[noreturn]] void block() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

enum Kind : std::uint32_t { kNext, kBoot, kValue, kTagged, kRound, kExit };

struct Message {
    std::uint32_t to{0};
    Kind          kind{kValue};
    double        value{0};
    std::uint32_t channel{0};  // kTagged: the channel; kRound: the round's index
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto          s    = static_cast<std::uint64_t>(p.get("simulations"));
    const auto          c    = static_cast<std::uint32_t>(p.get("columns"));
    const auto          ch   = static_cast<std::uint32_t>(p.get("channels"));
    const std::string   mode = plant();
    const Coefficients  k(ch, c);
    std::vector<Tally>  t(actors(ch));
    std::vector<Delay>  d1(ch, Delay(c - 1)), d2(ch, Delay(c - 1));
    std::vector<Sampler> sm(ch, Sampler(c));
    std::vector<Fir>     f1, f2;
    for (std::uint32_t j = 0; j < ch; ++j) {
        const std::uint32_t h = mode == "swapped" && j < 2 && ch >= 2 ? 1 - j : j;
        f1.emplace_back(c, k.h(h));
        f2.emplace_back(c, k.f(j));
    }
    Join                             join(ch, rounds(p));
    std::vector<std::vector<double>> table;
    std::vector<std::uint64_t>       sequence(static_cast<std::size_t>(rounds(p)));
    Sequence                         out(sequence.data(), rounds(p));
    std::vector<std::uint64_t>       sent(actors(ch), 0);  // what each actor sent, for the plants
    std::uint64_t                    produced = 0, current = 0;
    bool                             ended    = false;
    std::deque<Message>              fifo;

    const auto send = [&](std::uint32_t to, Kind kind, double value = 0, std::uint32_t channel = 0) {
        fifo.push_back({to, kind, value, channel});
    };
    watch.start();
    send(kProducer, kNext);  // the reference's first Next
    while (!fifo.empty() && !ended) {
        const Message m = fifo.front();
        fifo.pop_front();
        Tally &a = t[m.to];
        if (m.kind == kExit) {
            ++a.exits;
            if (m.to == kSource) send(kBranches, kExit);
            else if (m.to == kBranches)
                for (std::uint32_t j = 0; j < ch; ++j) send(bank_actor(j, kBank), kExit);
            else if (m.to == integrator(ch)) {
                if (a.exits == ch) send(combiner(ch), kExit);
            } else if (m.to == combiner(ch)) send(sink(ch), kExit);
            else if (m.to == sink(ch)) {
                watch.stop();
                ended = true;
            } else {
                const std::uint32_t j = (m.to - 3) / kRoles, role = (m.to - 3) % kRoles;
                if (role == kTagger) {
                    if (!(mode == "lost-exit" && j == 2)) send(integrator(ch), kExit);
                } else {
                    send(m.to + 1, kExit);
                }
            }
            continue;
        }
        switch (m.kind) {
        case kNext:
            ++a.count;
            if (produced == s) send(kSource, kExit);
            else {
                ++produced;
                send(kSource, kBoot);
            }
            continue;
        case kBoot:
            ++a.count;
            send(kBranches, kValue, sample(current++));
            send(kProducer, kNext);
            continue;
        case kTagged: {
            a.add(m.value);
            if (!join.put(m.channel, m.value)) continue;
            bool more = true;
            while (more) {
                table.emplace_back(ch);
                more = join.take(table.back().data());
                send(combiner(ch), kRound, 0, static_cast<std::uint32_t>(table.size() - 1));
            }
            continue;
        }
        case kRound: {
            const std::vector<double> &round = table[m.channel];
            a.add_round(round.data(), ch);
            send(sink(ch), kValue, combine(round.data(), ch));
            continue;
        }
        default: break;
        }
        a.add(m.value);
        if (m.to == kBranches) {
            for (std::uint32_t j = 0; j < ch; ++j) send(bank_actor(j, kBank), kValue, m.value);
            continue;
        }
        if (m.to == sink(ch)) {
            out.take(m.value);
            continue;
        }
        const std::uint32_t j = (m.to - 3) / kRoles, role = (m.to - 3) % kRoles;
        const std::uint64_t n = ++sent[m.to];
        double              y = 0;
        switch (role) {
        case kBank: send(m.to + 1, kValue, m.value); break;
        case kDelay1:
            y = d1[j].step(m.value);
            send(m.to + 1, kValue, y);
            if (mode == "duplicate" && j == 0 && n == 100) send(m.to + 1, kValue, y);
            break;
        case kFir1:
            if (f1[j].step(m.value, y)) send(m.to + 1, kValue, y);
            break;
        case kSampler: {
            const bool keep = sm[j].keeps() || (mode == "untouched" && j == 2 && n == 2);
            if (!(mode == "lost" && j == 1 && n == 50)) send(m.to + 1, kValue, keep ? m.value : 0.0);
            break;
        }
        case kDelay2:
            y = d2[j].step(m.value);
            send(mode == "routed-around" && j == 0 ? m.to + 2 : m.to + 1, kValue, y);
            break;
        case kFir2:
            if (f2[j].step(m.value, y)) send(m.to + 1, kValue, y);
            break;
        default:  // the tagger
            send(integrator(ch), kTagged, m.value, mode == "misroute" && j == 0 && n == 5 ? 1 : j);
            break;
        }
    }
    if (!ended) block();

    // The closed form of every count (filterbank::expected_messages, actor by actor).
    const std::uint64_t o1 = first_outputs(p), o2 = rounds(p);
    bool                as_closed = t[kProducer].count == s + 1 && t[kSource].count == s
                     && t[kBranches].count == s && t[integrator(ch)].count == ch * o2
                     && t[combiner(ch)].count == o2 && t[sink(ch)].count == o2;
    for (std::uint32_t j = 0; j < ch; ++j) {
        const std::uint64_t want[kRoles] = {s, s, s, o1, o1, o1, o2};
        for (std::uint32_t r = 0; r < kRoles; ++r)
            as_closed = as_closed && t[bank_actor(j, static_cast<Role>(r))].count == want[r];
    }
    std::fprintf(stderr, "filterbank subject: every count as the closed form says: %s\n",
                 as_closed ? "yes" : "no");
    return qvo::Answer{checksum(t, sequence), delivered(t)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/filterbank";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::filterbank::params();
    spec.expected          = qvospec::savina::filterbank::expected;
    spec.expected_messages = qvospec::savina::filterbank::expected_messages;
    spec.work_unit         = qvospec::savina::filterbank::kWorkUnit;
    spec.work_units        = qvospec::savina::filterbank::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the actor graph simulated on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
