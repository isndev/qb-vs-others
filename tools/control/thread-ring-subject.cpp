// The negative control's thread-ring subject: savina/thread-ring's verification, watched rejecting
// the defects its checksum used to be blind to (Huly QB-1017).
//
// Until QB-1017 the ring folded mix(remaining) into the token and every adapter DECLARED
// `hops + 1` messages, so a token delivered twice (each copy carries its own countdown and a
// right total), a token delivered to the wrong ring actor and a token one actor forwards
// untouched all verified. Now each ring actor keeps a thread_ring::Tally -- how many tokens it
// received, and the sum of their countdowns -- and the checksum is the digest of every actor's
// tally against the closed form: the parameters fix both numbers for every actor.
//
// This is a ring of `actors` actors simulated on one thread -- a FIFO of messages, each actor a
// Tally -- that tallies through the spec's own Tally::add, counts the results its sink handles,
// reduces through the spec's checksum() and delivered(), and ends at the first result like every
// adapter's sink. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none           correct -- the POSITIVE control: must verify.
//   duplicate      the LAST delivery is handled twice, its copy carrying the right countdown.
//   misroute       the hop half-way through goes two actors on instead of one.
//   untouched      actor 1, on its first visit, passes the token on without its handler running.
//   never-touched  the ring routes around actor 1 on every lap: its tally stays empty.
//   stride-2       EVERY hop goes two actors on -- on an odd ring with hops a multiple of actors,
//                  a CONSISTENT misroute: every actor receives exactly as many tokens as it should,
//                  so a per-actor count alone would pass it, and only the sums see it.
//
// Each plant must be REJECTED by the harness. After a run the subject says on stderr whether every
// actor's count equals the closed form ("every count as the closed form says: yes"), which is how
// the battery shows the stride-2 plant is the case a count misses. Run with --param hops=0, the
// spec must refuse the run before anything is built (exit 2): a token with no hop left would
// count down from 2^64.
//
// Like the ping-pong subject beside it, it is NOT built by qvo_add_benchmark and its name does not
// start with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/thread-ring.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace {

using namespace qvospec::savina::thread_ring;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

struct Message {
    std::uint32_t dst{0};  // a ring index; ignored for a result
    bool          result{false};
    std::uint64_t remaining{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto          actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto          hops   = static_cast<std::uint64_t>(p.get("hops"));
    const std::string   mode   = plant();
    const std::uint32_t victim = actors > 2 ? 1 : 0;  // the actor two of the plants single out

    auto successor = [&](std::uint32_t i) {
        const std::uint32_t step = mode == "stride-2" ? 2 : 1;
        std::uint32_t       n    = (i + step) % actors;
        if (mode == "never-touched" && n == victim) n = n + 1 == actors ? 0 : n + 1;
        return n;
    };

    std::vector<Tally>  tallies(actors);
    std::uint64_t       results = 0;
    bool                planted = false;
    std::deque<Message> fifo;

    watch.start();
    fifo.push_back({0, false, hops});
    while (!fifo.empty()) {
        Message m = fifo.front();
        fifo.pop_front();
        if (m.result) {  // the sink: the first result ends the run, like every adapter's
            if (results++ == 0) {
                watch.stop();
                break;
            }
            continue;
        }
        if (mode == "untouched" && m.dst == victim && !planted) {
            planted = true;
            fifo.push_back({successor(m.dst), false, m.remaining});
            continue;
        }
        if (mode == "duplicate" && m.remaining == 1 && !planted) {
            planted = true;
            fifo.push_front(m);  // the same delivery, handled again right after this one
        }
        tallies[m.dst].add(m.remaining);
        if (--m.remaining == 0) {
            fifo.push_back({0, true, 0});
            continue;
        }
        std::uint32_t next = successor(m.dst);
        if (mode == "misroute" && m.remaining == hops / 2 && !planted) {
            planted = true;
            next    = successor(next);
        }
        fifo.push_back({next, false, m.remaining});
    }

    bool counts_as_expected = true;
    for (std::uint32_t i = 0; i < actors; ++i)
        counts_as_expected = counts_as_expected && tallies[i].count == expected_count(i, actors, hops);
    std::fprintf(stderr, "thread-ring subject: every count as the closed form says: %s\n",
                 counts_as_expected ? "yes" : "no");
    return qvo::Answer{checksum(tallies), delivered(tallies, results)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/thread-ring";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::thread_ring::params();
    spec.expected          = qvospec::savina::thread_ring::expected;
    spec.expected_messages = qvospec::savina::thread_ring::expected_messages;
    spec.work_unit         = qvospec::savina::thread_ring::kWorkUnit;
    spec.work_units        = qvospec::savina::thread_ring::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "a ring simulated on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
