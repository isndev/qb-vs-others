// The negative control's radixsort subject: savina/radixsort's verification, watched rejecting each
// fault class the shape can suffer (Huly QB-246; the thread-ring subject's method, Huly QB-1017).
//
// The pipeline -- source, `bits` stages, validation -- simulated on one thread: a FIFO of messages,
// each stage a radixsort::Stage over its Tally, the validation a radixsort::Validation, reduced
// through the spec's checksum() and delivered() like every adapter. `QVO_CONTROL_PLANT` makes it
// wrong in one way at a time:
//
//   none           correct -- the POSITIVE control: must verify.
//   duplicate      stage 3 sends its first passed value twice: stage 4 receives one value more than
//                  the source draws and the run FAILS (qvo::fail, a protocol violation).
//   lost           stage 3 drops its tenth passed value: stage 4 never completes, the run BLOCKS
//                  and the harness's deadline fails it.
//   skip           stage 3 sends its tenth passed value to stage 5: the same block (a misroute in a
//                  linear chain leaves the stage it skipped short).
//   untouched      the last stage passes its first held value on at once: the sequence is unsorted.
//   routed-around  the source sends to stage 1: stage 0 never receives a value (never-touched work).
//   reversed       the last stage flushes what it held in reverse: the sort is no longer stable.
//   swapped        stages 0 and 1 are wired in each other's place -- a CONSISTENT misroute: every
//                  stage still receives every value and holds exactly its own bit's ones, so every
//                  tally is right, and only the sequence sees it.
//
// Each plant must be REJECTED. After a run the subject says on stderr whether every stage's tally
// equals the closed form ("every stage tally as the closed form says: yes"), which is how the
// battery shows the swapped plant is the case the tallies miss. Run with --param values=0, the
// spec must refuse the run before anything is built (exit 2).
//
// Like the other subjects it is NOT built by qvo_add_benchmark and its name does not start with
// "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/radixsort.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qvospec::savina::radixsort;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

// A planted loss leaves the pipeline waiting forever, as a framework's would: the deadline ends it.
[[noreturn]] void block() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

struct Message {
    std::uint32_t to{0};  // 1 + k: stage k; bits + 1: the validation
    std::uint64_t value{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto        values = static_cast<std::uint64_t>(p.get("values"));
    const auto        bits   = static_cast<std::uint32_t>(p.get("bits"));
    const std::string mode   = plant();
    const std::uint32_t last = bits + 1;

    // The chain's order: position 1 + k is stage k, except where a plant rewires it.
    std::vector<std::uint32_t> stage_at(bits + 2);
    for (std::uint32_t k = 0; k < bits; ++k) stage_at[k + 1] = k;
    if (mode == "swapped" && bits >= 2) {
        stage_at[1] = 1;
        stage_at[2] = 0;
    }

    std::vector<Tally>                      tallies(bits);
    std::vector<Stage>                      stages(bits);
    std::vector<std::vector<std::uint64_t>> held(bits);
    for (std::uint32_t k = 0; k < bits; ++k) stages[k] = Stage(k, values, tallies[k]);
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(values));
    Validation                 validation(sequence.data(), values);
    std::uint64_t              starts   = 0;
    std::uint64_t              passed3  = 0;  // what stage 3 passed on, for its plants
    bool                       complete = false;
    std::deque<Message>        fifo;

    const std::uint32_t victim = bits > 3 ? 3 : 0;
    watch.start();
    ++starts;  // the source's Start
    {
        Source              source(p.get("seed"), p.get("bits"));
        const std::uint32_t first = mode == "routed-around" && bits >= 2 ? 2 : 1;
        for (std::uint64_t i = 0; i < values; ++i) fifo.push_back({first, source.next()});
    }
    while (!fifo.empty() && !complete) {
        const Message m = fifo.front();
        fifo.pop_front();
        if (m.to == last) {
            if (validation.take(m.value)) {
                watch.stop();
                complete = true;
            }
            continue;
        }
        const std::uint32_t k    = stage_at[m.to];
        const std::uint32_t next = m.to + 1;
        if (stages[k].take(m.value)) {
            std::uint32_t to = next;
            if (k == victim && bits > 5) {
                ++passed3;
                if (mode == "duplicate" && passed3 == 1) fifo.push_back({next, m.value});
                if (mode == "lost" && passed3 == 10) to = 0;
                if (mode == "skip" && passed3 == 10) to = next + 1;
            }
            if (to != 0) fifo.push_back({to, m.value});
        } else if (mode == "untouched" && k == bits - 1 && held[k].empty() && tallies[k].held == 1) {
            fifo.push_back({next, m.value});  // passed on at once, as if its bit were 0
            held[k].push_back(~std::uint64_t{0});  // marks the plant spent; never flushed
        } else {
            held[k].push_back(m.value);
        }
        if (stages[k].complete()) {
            std::vector<std::uint64_t> out;
            for (const std::uint64_t h : held[k])
                if (h != ~std::uint64_t{0}) out.push_back(h);
            if (mode == "reversed" && k == bits - 1) out.assign(out.rbegin(), out.rend());
            for (const std::uint64_t h : out) fifo.push_back({next, h});
        }
    }
    if (!complete) block();

    const auto in        = input(p);
    bool       as_closed = true;
    for (std::uint32_t k = 0; k < bits; ++k) {
        const Counts e = expected_counts(in, k);
        as_closed      = as_closed && tallies[k].count == e.count && tallies[k].sum == e.sum
                    && tallies[k].held == e.held;
    }
    std::fprintf(stderr, "radixsort subject: every stage tally as the closed form says: %s\n",
                 as_closed ? "yes" : "no");
    return qvo::Answer{checksum(tallies, sequence), delivered(tallies, validation.count(), starts)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/radixsort";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::radixsort::params();
    spec.expected          = qvospec::savina::radixsort::expected;
    spec.expected_messages = qvospec::savina::radixsort::expected_messages;
    spec.work_unit         = qvospec::savina::radixsort::kWorkUnit;
    spec.work_units        = qvospec::savina::radixsort::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the pipeline simulated on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
