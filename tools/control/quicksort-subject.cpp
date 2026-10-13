// The negative control's quicksort subject: savina/quicksort's verification, watched rejecting the
// defects a sorting run can hide (Huly QB-246).
//
// The verification is three things reduced after the window -- the driver's sequence measured
// (length, multiset of its values, descents) and the Sorts and Results the actors counted -- against
// the sequential reference of the same tree. This is that tree run on ONE thread: a FIFO of
// messages, each actor a node, through the spec's own split(), sort_leaf(), join(), Counts,
// measure(), answer() and explain(), ending when the driver receives the root's Result like every
// adapter's. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none           correct -- the POSITIVE control: must verify.
//   dup-result     the 10th Result is handled twice by its parent: its values joined twice.
//   wrong-side     the 10th Result is joined on the wrong side of what its parent holds -- every
//                  value present, the sequence out of order.
//   unsorted-leaf  the first leaf answers without its sequential sort: every value present, one
//                  run of them unsorted.
//   inline-split   the first actor below the root that should split sorts its sequence itself
//                  instead: the RIGHT output, reached with fewer actors -- only the counts see it.
//   lost-result    the 10th Result is never delivered: its parent never completes, nothing ends
//                  the run, and the harness's deadline must fail it.
//
// Each plant must be REJECTED by the harness. After a run the subject says on stderr whether the
// driver's sequence is the sorted input ("the output is the sorted input: yes"), which is how the
// battery shows the inline-split plant is the case an output check alone misses. Run with n below
// threshold, the spec must refuse the run before anything is built (exit 2).
//
// Like the other subjects beside it, it is NOT built by qvo_add_benchmark and its name does not
// start with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/quicksort.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qvospec::savina::quicksort;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

constexpr std::uint32_t kDriver = 0xffffffffu;

struct Node {
    std::uint32_t parent{kDriver};
    Position      position{Position::kInitial};
    Values        held;
    Counts        counts;
    std::uint32_t fragments{0};
    std::uint32_t depth{0};
};

struct Message {
    std::uint32_t dst{0};
    bool          result{false};
    Values        data;
    Counts        counts;
    Position      position{Position::kInitial};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const std::string mode      = plant();
    const auto        threshold = static_cast<std::size_t>(p.get("threshold"));
    Values            in        = input(p);

    std::vector<Node>   nodes(1);  // the root, index 0
    std::deque<Message> fifo;
    Values              output;
    Counts              total;
    bool                done        = false;
    bool                leaf_plant  = false;
    bool                split_plant = false;
    std::uint64_t       results     = 0;

    auto answer_parent = [&](std::uint32_t self) {
        Node &n = nodes[self];
        fifo.push_back({n.parent, true, std::move(n.held), n.counts, n.position});
    };

    watch.start();
    fifo.push_back({0, false, std::move(in), {}, Position::kInitial});
    while (!fifo.empty()) {
        Message m = std::move(fifo.front());
        fifo.pop_front();
        if (m.result) {
            ++results;
            if (results == 10 && mode == "lost-result") continue;  // never delivered
            const int times = results == 10 && mode == "dup-result" ? 2 : 1;
            for (int t = 0; t < times; ++t) {
                if (m.dst == kDriver) {
                    total.add_result(m.counts);
                    output = m.data;
                    done   = true;
                    watch.stop();
                    continue;
                }
                Node    &n    = nodes[m.dst];
                Position from = m.position;
                if (results == 10 && mode == "wrong-side")
                    from = from == Position::kLeft ? Position::kRight : Position::kLeft;
                n.counts.add_result(m.counts);
                join(n.held, m.data, from);
                if (++n.fragments == 3) answer_parent(m.dst);
            }
            continue;
        }
        Node &n = nodes[m.dst];
        ++n.counts.sorts;
        Values data = std::move(m.data);
        const bool inline_here =
            mode == "inline-split" && !split_plant && n.depth == 1 && data.size() >= threshold;
        if (data.size() < threshold || inline_here) {
            if (inline_here) split_plant = true;
            if (mode == "unsorted-leaf" && !leaf_plant) leaf_plant = true;
            else sort_leaf(data);
            n.held = std::move(data);
            answer_parent(m.dst);
            continue;
        }
        Split               s     = split(data);
        const std::uint32_t self  = m.dst;
        const std::uint32_t depth = n.depth;
        const auto          left  = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back({self, Position::kLeft, {}, {}, 0, depth + 1});
        nodes.push_back({self, Position::kRight, {}, {}, 0, depth + 1});
        fifo.push_back({left, false, std::move(s.less), {}, Position::kInitial});
        fifo.push_back({left + 1, false, std::move(s.greater), {}, Position::kInitial});
        nodes[self].held      = std::move(s.equal);
        nodes[self].fragments = 1;
    }
    if (!done) {
        // A message the protocol waits for was lost: nothing ends the run. Block like every adapter
        // would, and let the harness's deadline fail the repetition.
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    const Output o      = measure(output);
    const auto  &r      = reference(p);
    const bool   sorted = o.length == static_cast<std::uint64_t>(r.n) && o.multiset == r.multiset
                        && o.descents == 0;
    std::fprintf(stderr, "quicksort subject: the output is the sorted input: %s\n", sorted ? "yes" : "no");
    explain(o, total, p);
    return qvo::Answer{answer(o, total), total.sorts + total.results};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/quicksort";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::quicksort::params();
    spec.expected          = qvospec::savina::quicksort::expected;
    spec.expected_messages = qvospec::savina::quicksort::expected_messages;
    spec.work_unit         = qvospec::savina::quicksort::kWorkUnit;
    spec.work_units        = qvospec::savina::quicksort::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the quicksort tree run on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
