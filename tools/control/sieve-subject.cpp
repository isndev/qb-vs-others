// The negative control's sieve subject: savina/sieve's verification, watched rejecting each fault
// class the shape can suffer (Huly QB-246; the thread-ring subject's method, Huly QB-1017).
//
// The growing chain -- a producer and the filters it grows -- simulated on one thread: a FIFO of
// messages, every filter a sieve::Filter over its Tally and its primes' vector, reduced through the
// spec's checksum() and delivered() like every adapter. `QVO_CONTROL_PLANT` makes it wrong in one
// way at a time:
//
//   none          correct -- the POSITIVE control: must verify.
//   duplicate     the producer sends the candidate 99 twice.
//   lost          the producer never sends the prime 97.
//   skip          filter 1 sends its third passed candidate to filter 3 (a misroute).
//   reorder       the producer sends 9 before 3: 9 reaches the end of the chain before its factor
//                 and is kept as a prime.
//   untouched     filter 2 passes every candidate on without testing it (never-touched work).
//   early-exit    the producer's Exit overtakes its last ten candidates.
//   grow-early    filter 0 creates its successor one prime early: the chain needs one filter more
//                 than the primes allow, and the run FAILS (qvo::fail, a protocol violation).
//   lost-exit     filter 5 drops the Exit: the run BLOCKS and the harness's deadline fails it.
//
// Each plant must be REJECTED. Run with --param local=0, the spec must refuse the run before
// anything is built (exit 2).
//
// Like the other subjects it is NOT built by qvo_add_benchmark and its name does not start with
// "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/sieve.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qvospec::savina::sieve;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

[[noreturn]] void block() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

enum Kind : std::uint32_t { kCandidate, kExit };

struct Message {
    std::uint32_t to{0};  // a filter
    Kind          kind{kCandidate};
    std::uint64_t number{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto        limit = static_cast<std::uint64_t>(p.get("limit"));
    const auto        local = static_cast<std::uint64_t>(p.get("local"));
    const auto        nfilt = static_cast<std::size_t>(filters(p));
    const std::string mode  = plant();

    std::vector<Tally>                      tallies(nfilt);
    std::vector<std::vector<std::uint64_t>> primes(nfilt);
    std::vector<std::unique_ptr<Filter>>    chain;
    std::vector<std::uint64_t>              passed(nfilt, 0);
    chain.push_back(std::make_unique<Filter>(2, mode == "grow-early" && local > 1 ? local - 1 : local,
                                             tallies[0], primes[0]));
    std::uint64_t       starts = 1;  // the producer's Start
    bool                ended  = false;
    std::deque<Message> fifo;

    watch.start();
    {
        std::vector<std::uint64_t> sends;
        for (std::uint64_t n = 3; n < limit; n += 2) {
            if (mode == "lost" && n == 97) continue;
            sends.push_back(n);
            if (mode == "duplicate" && n == 99) sends.push_back(n);
        }
        if (mode == "reorder" && sends.size() > 3) std::swap(sends[0], sends[3]);  // 3 and 9
        const std::size_t exit_at = mode == "early-exit" && sends.size() > 10 ? sends.size() - 10
                                                                              : sends.size();
        for (std::size_t i = 0; i < sends.size(); ++i) {
            if (i == exit_at) fifo.push_back({0, kExit, 0});
            fifo.push_back({0, kCandidate, sends[i]});
        }
        if (exit_at == sends.size()) fifo.push_back({0, kExit, 0});
    }
    std::size_t deferred = 0;  // messages in a row sent to a filter not created yet
    while (!fifo.empty() && !ended) {
        const Message m = fifo.front();
        fifo.pop_front();
        if (m.to >= chain.size()) {  // only a misrouted message: it waits for its filter, if ever
            if (m.to >= nfilt) continue;
            if (++deferred > fifo.size()) block();
            fifo.push_back(m);
            continue;
        }
        deferred  = 0;
        Filter &f = *chain[m.to];
        if (m.kind == kExit) {
            ++tallies[m.to].exits;
            if (!f.linked()) {
                watch.stop();
                ended = true;
            } else if (!(mode == "lost-exit" && m.to == 5)) {
                fifo.push_back({m.to + 1, kExit, 0});
            }
            continue;
        }
        if (mode == "untouched" && m.to == 2 && f.linked()) {  // passed on, never tested
            ++tallies[2].received;
            tallies[2].sum += m.number;
            ++tallies[2].forwarded;
            fifo.push_back({3, kCandidate, m.number});
            continue;
        }
        switch (f.take(m.number)) {
        case Filter::Verdict::pass: {
            const std::uint64_t n  = ++passed[m.to];
            const std::uint32_t to = mode == "skip" && m.to == 1 && n == 3 ? m.to + 2 : m.to + 1;
            fifo.push_back({to, kCandidate, m.number});
            break;
        }
        case Filter::Verdict::grow:
            if (m.to + 1 >= nfilt) too_many_filters();
            chain.push_back(std::make_unique<Filter>(m.number, local, tallies[m.to + 1],
                                                     primes[m.to + 1]));
            break;
        default: break;
        }
    }
    if (!ended) block();
    return qvo::Answer{checksum(tallies, primes), delivered(tallies, starts)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/sieve";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::sieve::params();
    spec.expected          = qvospec::savina::sieve::expected;
    spec.expected_messages = qvospec::savina::sieve::expected_messages;
    spec.work_unit         = qvospec::savina::sieve::kWorkUnit;
    spec.work_units        = qvospec::savina::sieve::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the growing chain simulated on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
