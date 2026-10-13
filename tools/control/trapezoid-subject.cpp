// The negative control's trapezoid subject: savina/trapezoid's verification, watched rejecting each
// fault class the shape can have and accepting an arrival order no framework controls (Huly QB-246).
//
// A master and `workers` workers simulated on one thread -- the work messages and the results each
// a FIFO -- that integrate through the spec's own integrate(), record through its Record, fold
// through fold(), and reduce through checksum() and delivered(), like every adapter.
// `QVO_CONTROL_PLANT` makes it wrong in one way at a time (the victim is worker 1):
//
//   none              correct -- the POSITIVE control: must verify.
//   reverse           correct, the results folded in REVERSE arrival order: must verify -- the
//                     master folds its records in worker order, so no arrival order moves a bit
//                     of the answer (Savina's master folds in arrival order).
//   duplicate-work    the victim's work message delivered twice: two results, twice its area.
//   duplicate-result  the victim's result delivered twice: the master's count is reached one real
//                     result early, and the last worker's record stays empty.
//   misroute          worker 2's work message delivered to the victim: the victim answers twice,
//                     worker 2 never -- every count of messages right, the records wrong.
//   untouched         the victim answers without integrating: an area of 0.
//   wrong-range       the victim integrates its range shifted by one trapezoid.
//
// Each plant must be REJECTED by the checksum. Run with --param workers=0 (or fewer pieces than
// workers), the spec must refuse the run before anything is built (exit 2).
//
// Like the subjects beside it, it is NOT built by qvo_add_benchmark and its name does not start
// with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/trapezoid.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace {

using namespace qvospec::savina::trapezoid;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

struct Work {
    std::uint32_t worker{0};
    std::uint64_t begin{0};
    std::uint64_t end{0};
};
struct Result {
    std::uint32_t worker{0};
    double        area{0.0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config        c      = config(p);
    const std::string   mode   = plant();
    const std::uint32_t victim = c.workers > 2 ? 1 : 0;

    std::vector<Record> records(c.workers);
    std::vector<Tally>  tallies(c.workers);
    double              total    = 0.0;
    std::uint64_t       received = 0;
    std::deque<Work>    works;
    std::vector<Result> results;

    watch.start();
    for (std::uint32_t w = 0; w < c.workers; ++w) {
        Work m{w, first_piece(w, c), first_piece(w + 1, c)};
        if (mode == "misroute" && w == victim + 1 && victim + 1 < c.workers) m.worker = victim;
        works.push_back(m);
        if (mode == "duplicate-work" && w == victim) works.push_back(m);
    }
    while (!works.empty()) {
        const Work m = works.front();
        works.pop_front();
        ++tallies[m.worker].works;
        double area = integrate(m.begin, m.end, c.left, c.h);
        if (m.worker == victim && mode == "untouched") area = 0.0;
        if (m.worker == victim && mode == "wrong-range")
            area = integrate(m.begin + 1, m.end + 1, c.left, c.h);
        results.push_back({m.worker, area});
        if (m.worker == victim && mode == "duplicate-result") results.push_back({m.worker, area});
    }
    if (mode == "reverse") std::reverse(results.begin(), results.end());
    for (const Result &r : results) {  // the master: the run ends at its `workers`-th result
        records[r.worker].add(r.area);
        if (++received == c.workers) {
            total = fold(records);
            watch.stop();
            break;
        }
    }
    return qvo::Answer{checksum(records, total, tallies), delivered(records, tallies)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/trapezoid";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::trapezoid::params();
    spec.expected          = qvospec::savina::trapezoid::expected;
    spec.expected_messages = qvospec::savina::trapezoid::expected_messages;
    spec.work_unit         = qvospec::savina::trapezoid::kWorkUnit;
    spec.work_units        = qvospec::savina::trapezoid::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "a master and its workers simulated on one thread; plants a defect "
                             "selected by QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
