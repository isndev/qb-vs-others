// The negative control's sor subject: savina/sor's verification, watched rejecting the defects it
// exists for (Huly QB-247).
//
// The grid of cells and its two owners simulated on ONE thread -- a pool of messages in flight,
// each cell a savina::sor::Cell, each owner a savina::sor::Owner -- driven exactly as every
// adapter drives them: the runner starts its cells and the peer, the peer its cells; a cell
// accept()s values, update()s, publishes to every neighbour and reports to its owner; the peer
// reports its half to the runner; the answer is the spec's checksum() and delivered().
// `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none         correct, FIFO delivery -- the POSITIVE control: must verify.
//   shuffle      correct, every message delivered in a seeded random order (no FIFO at all): must
//                verify -- with the fixed-point values the answer does not depend on the order the
//                neighbours' values are added in, where Savina's doubles would.
//   duplicate    one value, half-way through, delivered twice.
//   lost         one value, half-way through, never delivered: its receiver never completes the
//                round, and the run blocks -- the subject waits like an adapter would.
//   misroute     one value delivered to a cell that is not its sender's neighbour.
//   wrong-round  one value tagged with the generation after the one it carries.
//   off-by-one   one cell's value off by one unit at one update, half-way through the run: every
//                count is right, and every final value too -- the relaxation contracts and the
//                floor rounds, so the error dies out before the end; the witnesses see it.
//
// Each plant but the two correct ones must be REJECTED by the harness. Like the other subjects it
// is NOT built by qvo_add_benchmark and its name does not start with "qvo-": tools/run.py cannot
// discover it, and no planted result can reach a table.

#include <qvospec/savina/sor.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qvospec::savina::sor;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

struct Message {
    enum Kind { kStart, kPeerStart, kValue, kReport, kHalf } kind{kStart};
    std::uint32_t dst{0};  // a cell, for kStart and kValue; the owner's side, for kReport
    std::uint32_t generation{0};
    std::uint32_t from{0};
    std::int64_t  value{0};
    std::uint32_t updates{0};
    std::uint64_t received{0};
    std::uint64_t work{0};
};

[[noreturn]] void block_forever() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape       s    = shape(p);
    const std::string mode = plant();

    const std::vector<std::int64_t> start = initial(s);
    std::vector<Cell>               cells;
    cells.reserve(s.cells);
    for (std::uint32_t c = 0; c < s.cells; ++c) cells.emplace_back(s, c, start[c]);
    Owner runner(s, true);
    Owner peer(s, false);

    const std::uint64_t target    = edges(s) * (2ull * s.iterations - 1) / 2;
    std::uint64_t       published = 0;
    bool                planted   = false;

    std::deque<Message> pool;
    std::uint64_t       lcg = 0x9e3779b97f4a7c15ULL;
    auto next_message = [&]() {
        if (mode != "shuffle") {
            Message m = pool.front();
            pool.pop_front();
            return m;
        }
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::size_t i = static_cast<std::size_t>((lcg >> 33) % pool.size());
        Message           m = pool[i];
        pool[i]             = pool.back();
        pool.pop_back();
        return m;
    };

    auto publish = [&](const Cell &from) {
        std::int64_t v = from.value();
        if (mode == "off-by-one" && !planted && published >= target) {
            planted = true;
            ++v;  // to all of this update's neighbours alike: a value, not a delivery, is wrong
        }
        from.for_each_neighbour([&](std::uint32_t to) {
            Message m{Message::kValue, to, from.generation(), from.index(), v, 0, 0, 0};
            if (published++ == target && !planted) {
                planted = true;
                if (mode == "lost") return;
                if (mode == "duplicate") pool.push_back(m);
                if (mode == "wrong-round") m.generation += 1;
                if (mode == "misroute") m.dst = (to + 2 * s.size + 1) % s.cells;
            }
            pool.push_back(m);
        });
    };
    auto report = [&](Cell &c) {
        if (c.report_due())
            pool.push_back({Message::kReport, c.index(), 0, c.index(), c.value(), c.updates(),
                            c.received(), c.work()});
    };

    bool half = false;
    watch.start();
    for (std::uint32_t c = 0; c < s.cells; ++c)
        if (runner_owns(s, c)) pool.push_back({Message::kStart, c});
    pool.push_back({Message::kPeerStart, 0});
    while (!pool.empty()) {
        const Message m = next_message();
        switch (m.kind) {
        case Message::kPeerStart:
            peer.count();
            for (std::uint32_t c = 0; c < s.cells; ++c)
                if (!runner_owns(s, c)) pool.push_back({Message::kStart, c});
            break;
        case Message::kStart: {
            Cell &c = cells[m.dst];
            if (c.start()) publish(c);
            report(c);
            break;
        }
        case Message::kValue: {
            Cell &c = cells[m.dst];
            if (c.accept(m.generation, m.from, m.value)) {
                c.update();
                if (c.publishes()) publish(c);
            }
            report(c);
            break;
        }
        case Message::kReport:
            if (runner_owns(s, m.dst)) {
                runner.record(m.dst, m.value, m.updates, m.received, m.work);
            } else if (peer.record(m.dst, m.value, m.updates, m.received, m.work)) {
                pool.push_back({Message::kHalf, 0});
            }
            break;
        case Message::kHalf:
            runner.count();
            half = true;
            break;
        }
        if (half && runner.complete()) {
            watch.stop();
            break;
        }
    }
    if (!(half && runner.complete())) block_forever();

    // What a check WITHOUT the witnesses would have seen, said on stderr so the battery can show
    // that a plant passes it: the counts against their closed form, the final values against the
    // reference (computed here, outside the window).
    const Reference ref                = reference(s);
    bool            counts_as_expected = true;
    bool            values_as_expected = true;
    for (std::uint32_t c = 0; c < s.cells; ++c) {
        const Report       &r       = runner_owns(s, c) ? runner.report(c) : peer.report(c);
        const std::uint64_t updates = black(s, c) ? s.iterations - 1 : s.iterations;
        counts_as_expected = counts_as_expected && r.updates == updates
                             && r.received == 1 + cells[c].degree() * updates;
        values_as_expected = values_as_expected && r.value == ref.value[c];
    }
    std::fprintf(stderr, "sor subject: every cell's updates and receipts as the closed form says: %s\n",
                 counts_as_expected ? "yes" : "no");
    std::fprintf(stderr, "sor subject: every final value as the reference says: %s\n",
                 values_as_expected ? "yes" : "no");
    return qvo::Answer{checksum(s, runner, peer), delivered(s, runner, peer)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/sor";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::sor::params();
    spec.expected          = qvospec::savina::sor::expected;
    spec.expected_messages = qvospec::savina::sor::expected_messages;
    spec.work_unit         = qvospec::savina::sor::kWorkUnit;
    spec.work_units        = qvospec::savina::sor::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the cell grid and its two owners simulated on one thread; plants a "
                             "defect selected by QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
