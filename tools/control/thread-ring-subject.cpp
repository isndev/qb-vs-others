// The negative control's thread-ring subject: savina/thread-ring's verification, watched rejecting
// the defects its checksum used to be blind to (Huly QB-1017).
//
// Until QB-1017 every term of the ring's sum depended on the hop count alone and every adapter
// DECLARED `hops + 1` messages instead of counting them, so three defects verified: a token
// delivered twice (each copy carries its own countdown and a right total), a token delivered to the
// wrong ring actor, and a token one actor forwards untouched -- the next one folds the same term.
// The spec now names, in every term, the actor that folds it (thread_ring::term), and every adapter
// counts what its handlers ran.
//
// This is a ring of `actors` actors simulated on one thread -- a FIFO of messages, each actor a
// count -- that folds through the spec's own term(), counts the way every adapter counts (each
// ring actor the tokens its handler ran, the sink the results its handler ran) and ends at the
// first result like every adapter's sink. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none           correct -- the POSITIVE control: must verify.
//   duplicate      the LAST delivery is handled twice: the copy's total is the right one and it
//                  reaches the sink second, so only the count can see it (one delivery too many).
//   misroute       the hop half-way through goes two actors on instead of one: every count is
//                  right, the sum moves from that hop on.
//   untouched      actor 1, on its first visit, passes the token on without its handler running --
//                  no fold, no count, no decrement: the next actor folds that hop under its own
//                  index.
//   never-touched  the ring routes around actor 1 on every lap: it never receives the token.
//
// Each plant must be REJECTED by the harness. Run with --param hops=0, the spec must refuse the run
// before anything is built (exit 2): a token with no hop left would count down from 2^64.
//
// Like the ping-pong subject beside it, it is NOT built by qvo_add_benchmark and its name does not
// start with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/thread-ring.h>

#include <cstdint>
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
    std::uint64_t acc{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto        actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto        hops   = static_cast<std::uint64_t>(p.get("hops"));
    const std::string mode   = plant();
    const std::uint32_t victim = actors > 2 ? 1 : 0;  // the actor the last two plants single out

    auto successor = [&](std::uint32_t i) {
        std::uint32_t n = i + 1 == actors ? 0 : i + 1;
        if (mode == "never-touched" && n == victim) n = n + 1 == actors ? 0 : n + 1;
        return n;
    };

    std::vector<std::uint64_t> received(actors, 0);
    std::uint64_t              results  = 0;
    std::uint64_t              checksum = 0;
    bool                       planted  = false;
    std::deque<Message>        fifo;

    watch.start();
    fifo.push_back({0, false, hops, 0});
    while (!fifo.empty()) {
        Message m = fifo.front();
        fifo.pop_front();
        if (m.result) {  // the sink: the first result ends the run, like every adapter's
            if (results++ == 0) {
                watch.stop();
                checksum = m.acc;
                break;
            }
            continue;
        }
        if (mode == "untouched" && m.dst == victim && !planted) {
            planted = true;
            fifo.push_back({successor(m.dst), false, m.remaining, m.acc});
            continue;
        }
        if (mode == "duplicate" && m.remaining == 1 && !planted) {
            planted = true;
            fifo.push_front(m);  // the same delivery, handled again right after this one
        }
        ++received[m.dst];
        m.acc += term(m.dst, m.remaining);
        if (--m.remaining == 0) {
            fifo.push_back({0, true, 0, m.acc});
            continue;
        }
        std::uint32_t next = successor(m.dst);
        if (mode == "misroute" && m.remaining == hops / 2 && !planted) {
            planted = true;
            next    = successor(next);
        }
        fifo.push_back({next, false, m.remaining, m.acc});
    }

    std::uint64_t messages = results;
    for (const std::uint64_t n : received) messages += n;
    return qvo::Answer{checksum, messages};
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
