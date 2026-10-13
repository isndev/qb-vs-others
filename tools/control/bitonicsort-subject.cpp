// The negative control's bitonicsort subject: savina/bitonicsort's verification, watched rejecting
// the defects a sorting network can hide (Huly QB-246).
//
// The verification is every actor's tally -- messages received, the wrapping sum of the values they
// carried -- against the reference network's, plus the sink's sequence measured (length, multiset,
// descents), all reduced after the window. This is the network of the spec's topology() run on ONE
// thread: a FIFO of messages through the spec's own Splitter, Comparator and Joiner, tallying
// through its Counts, reduced through its checksum() and delivered(), ending at the sink's first
// Exit like every adapter's sink. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none        correct -- the POSITIVE control: must verify.
//   dup-sink    the last value the sink receives is handled twice.
//   swap        the first compare-exchange whose two values differ sends them in the wrong order. The
//               network still SORTS (the later stages repair it, measured: 0 descents) -- only the
//               tallies of the actors downstream of it see the defect.
//   untouched   the network's first compare-exchange passes its two values on, stamped, without its
//               handler running: the output is still sorted, only its own tally is short.
//   stride      a partition splitter half-way down the pipeline pairs neighbours (values 2k and
//               2k + 1 to compare-exchange k) instead of values k and k + L/2: a CONSISTENT misroute
//               -- every compare-exchange receives exactly two values, so every count stays right
//               and only the sums see it.
//   lost-value  the network's first splitter drops one value: no actor blocks -- the Exit wave
//               still closes every joiner -- so the run ENDS, short of every value the stalled
//               lane held back.
//   dup-exit    the joiner feeding the sink's adapter sends an Exit on its first lane's Exit as well
//               as on its last: a control message too many.
//   dup-data    the first stamped value a joiner receives is handled twice: its lane overflows, which
//               the spec's Joiner stops with a named failure (qvo::fail).
//   lost-exit   one compare-exchange's Exit is never delivered: its joiner never closes, nothing ends
//               the run, and the harness's deadline must fail it.
//
// Each plant must be REJECTED by the harness; a plant whose victim the topology does not have is
// refused (exit 2), never run as a correct network. After a run the spec's explain() line says
// whether every actor's COUNT is the reference's ("every count as the reference says: yes"), which
// is how the battery shows the stride and swap plants are the cases a per-actor count -- and, for
// swap, the output check -- miss. Run with n not a power of two, the spec must refuse the run before
// anything is built (exit 2).
//
// Like the other subjects beside it, it is NOT built by qvo_add_benchmark and its name does not
// start with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/bitonicsort.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace qvospec::savina::bitonicsort;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

enum class Msg : std::uint8_t { kStart, kValue, kData, kExit };
struct M {
    std::uint32_t dst;
    Msg           kind;
    std::uint32_t order;
    std::uint64_t v;
};

// The plants' victims, chosen from the topology alone.
struct Victims {
    std::uint32_t first_compare{kNone};    // a compare-exchange of the first step
    std::uint32_t first_splitter{kNone};   // the first step's splitter
    std::uint32_t stride_splitter{kNone};  // a partition splitter of >= 2 compare-exchanges, mid-pipeline
    std::uint32_t sink_joiner{kNone};      // the joiner whose output the sink's adapter receives
};
Victims victims(const Topology &net, const std::string &mode) {
    Victims v;
    for (std::uint32_t i = 0; i < net.nodes.size(); ++i) {
        const Node &node = net.nodes[i];
        if (node.kind == Kind::kCompare && node.step == 0) v.first_compare = i;
        if (node.kind == Kind::kSplitter && node.step == 0) v.first_splitter = i;
        if (node.kind == Kind::kSplitter && node.length == 1 && node.lanes >= 2
            && node.step >= net.steps / 2 && v.stride_splitter == kNone)
            v.stride_splitter = i;
        if (node.kind == Kind::kJoiner && node.next == 1) v.sink_joiner = i;
    }
    const bool missing = ((mode == "untouched" || mode == "lost-exit") && v.first_compare == kNone)
                         || (mode == "lost-value" && v.first_splitter == kNone)
                         || (mode == "stride" && v.stride_splitter == kNone)
                         || (mode == "dup-exit" && v.sink_joiner == kNone);
    if (missing)
        qvo::refuse("bitonicsort subject: the plant '" + mode + "' has no victim in a network of "
                    + std::to_string(net.n) + " -- refusing to run it as a correct network");
    return v;
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const std::string mode = plant();
    const Values      in   = input(p);
    const Reference  &ref  = reference(p);
    const Topology   &net  = ref.net;
    const auto        size = net.nodes.size();
    const Victims     vic  = victims(net, mode);

    std::vector<Tally>         tallies(size);
    std::vector<Splitter>      splitters(size);
    std::vector<Comparator>    comparators(size);
    std::vector<Joiner>        joiners;
    std::vector<std::uint32_t> joiner_of(size, kNone);
    for (std::size_t i = 0; i < size; ++i)
        if (net.nodes[i].kind == Kind::kJoiner) {
            joiner_of[i] = static_cast<std::uint32_t>(joiners.size());
            joiners.emplace_back(net.nodes[i]);
        }

    std::uint32_t stride_run = 0;  // values the stride splitter dealt
    std::uint32_t first_dealt = 0;  // values the first splitter dealt
    bool          swapped = false, doubled = false, exit_lost = false, exit_doubled = false;
    std::uint64_t sink_values = 0;
    Values        out;
    out.reserve(in.size() + 1);
    bool done = false;

    std::deque<M> fifo;
    watch.start();
    fifo.push_back({net.source(), Msg::kStart, 0, 0});
    while (!fifo.empty() && !done) {
        const M m = fifo.front();
        fifo.pop_front();
        const Node &node = net.nodes[m.dst];
        const bool  exit = m.kind == Msg::kExit;

        if (mode == "untouched" && m.dst == vic.first_compare && !exit) {
            fifo.push_back({node.next, Msg::kData, node.order, m.v});  // no handler, no tally
            continue;
        }
        int times = 1;
        if (mode == "dup-sink" && node.kind == Kind::kSink && !exit && ++sink_values == in.size())
            times = 2;
        if (mode == "dup-data" && node.kind == Kind::kJoiner && m.kind == Msg::kData && !doubled) {
            doubled = true;
            times   = 2;
        }
        for (int t = 0; t < times && !done; ++t) {
            if (m.kind == Msg::kValue || m.kind == Msg::kData) tallies[m.dst].value(m.v);
            else tallies[m.dst].signal();
            switch (node.kind) {
            case Kind::kSource:
                for (const std::uint64_t v : in) fifo.push_back({node.next, Msg::kValue, 0, v});
                fifo.push_back({node.next, Msg::kExit, 0, 0});
                break;
            case Kind::kForward: fifo.push_back({node.next, m.kind, m.order, m.v}); break;
            case Kind::kSplitter:
                if (exit) {
                    for (std::uint32_t k = 0; k < node.lanes; ++k) {
                        const std::uint32_t r = net.receivers[node.first + k];
                        if (mode == "lost-exit" && r == vic.first_compare && !exit_lost) {
                            exit_lost = true;  // never delivered
                            continue;
                        }
                        fifo.push_back({r, Msg::kExit, 0, 0});
                    }
                } else {
                    std::uint32_t k = splitters[m.dst].next(node);
                    if (mode == "stride" && m.dst == vic.stride_splitter) {
                        // Value j of each run of 2 * lanes to compare-exchange j / 2, not j % lanes.
                        k = (stride_run++ % (2 * node.lanes)) / 2;
                    }
                    if (mode == "lost-value" && m.dst == vic.first_splitter && ++first_dealt == 5)
                        break;  // the fifth value, never delivered
                    fifo.push_back({net.receivers[node.first + k], Msg::kValue, 0, m.v});
                }
                break;
            case Kind::kCompare: {
                std::uint64_t a = 0, b = 0;
                if (exit) fifo.push_back({node.next, Msg::kExit, 0, 0});
                else if (comparators[m.dst].accept(m.v, node.up, a, b)) {
                    if (mode == "swap" && !swapped && a != b) {
                        swapped = true;
                        std::swap(a, b);
                    }
                    fifo.push_back({node.next, Msg::kData, node.order, a});
                    fifo.push_back({node.next, Msg::kData, node.order, b});
                }
                break;
            }
            case Kind::kJoiner:
                if (exit) {
                    const bool last  = joiners[joiner_of[m.dst]].close();
                    const bool extra = mode == "dup-exit" && m.dst == vic.sink_joiner && !exit_doubled;
                    if (extra) exit_doubled = true;
                    if (last || extra) fifo.push_back({node.next, Msg::kExit, 0, 0});
                } else {
                    joiners[joiner_of[m.dst]].accept(m.order, m.v, [&](std::uint64_t v) {
                        fifo.push_back({node.next, Msg::kValue, 0, v});
                    });
                }
                break;
            case Kind::kValueData:
                fifo.push_back({node.next, exit ? Msg::kExit : Msg::kData, exit ? 0 : node.order, m.v});
                break;
            case Kind::kDataValue:
                fifo.push_back({node.next, exit ? Msg::kExit : Msg::kValue, 0, m.v});
                break;
            case Kind::kSink:
                if (exit) {  // the first Exit ends the run, like every adapter's sink
                    watch.stop();
                    done = true;
                } else {
                    out.push_back(m.v);
                }
                break;
            }
        }
    }
    if (!done) {
        // A message the protocol waits for was lost: nothing ends the run. Block like every adapter
        // would, and let the harness's deadline fail the repetition.
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    explain(tallies, out, p);
    return qvo::Answer{checksum(tallies, out), delivered(tallies)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/bitonicsort";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::bitonicsort::params();
    spec.expected          = qvospec::savina::bitonicsort::expected;
    spec.expected_messages = qvospec::savina::bitonicsort::expected_messages;
    spec.work_unit         = qvospec::savina::bitonicsort::kWorkUnit;
    spec.work_units        = qvospec::savina::bitonicsort::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the bitonic network run on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
