// The negative control's apsp subject: savina/apsp's verification, watched rejecting the defects
// it exists for (Huly QB-247).
//
// The grid of blocks simulated on ONE thread -- a pool of messages in flight, each block a
// savina::apsp::Block over the spec's own Storage -- driven exactly as every adapter drives it:
// Initial publishes generation 0, a tile is accept()ed, compute()d while ready(), each new
// generation published to every neighbour in for_each_neighbour's order, a Done to the sink when
// a block has finished; the answer is the spec's checksum(), delivered() and ahead().
// `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none           correct, FIFO delivery -- the POSITIVE control: must verify.
//   shuffle        correct, every message delivered in a seeded random order (no FIFO at all):
//                  must verify -- the answer does not depend on the interleaving.
//   duplicate      one tile, half-way through, delivered twice.
//   lost           one tile, half-way through, never delivered: the phase it completes never
//                  completes, and the run blocks -- the subject waits like an adapter would.
//   misroute       one tile delivered to a block outside its sender's row and column.
//   misroute-row   one tile delivered to ANOTHER block of its sender's row: that block holds the
//                  sender's tile of that generation twice.
//   early          one block computes a phase holding all but one of its neighbours' tiles.
//   far-ahead      one tile tagged two generations ahead of the one it carries.
//   skip-change    one block throws away the first phase that changed its tile (282 distances at
//                  the defaults): every count is right, and so is the final matrix -- Floyd-
//                  Warshall reaches it anyway through later pivots and tied paths; the witness
//                  (apsp.h) sees it.
//
// Each plant but the two correct ones must be REJECTED by the harness. Like the other subjects it
// is NOT built by qvo_add_benchmark and its name does not start with "qvo-": tools/run.py cannot
// discover it, and no planted result can reach a table.

#include <qvospec/savina/apsp.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace qvospec::savina::apsp;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

struct Message {
    enum Kind { kInitial, kTile, kDone } kind{kInitial};
    std::uint32_t       dst{0};
    std::uint32_t       generation{0};
    std::uint32_t       from{0};
    const std::int64_t *tile{nullptr};
};

// What an adapter does when its protocol never ends: nothing. The harness's deadline ends it.
[[noreturn]] void block_forever() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape       s    = shape(p);
    const std::string mode = plant();

    Storage            storage(s);
    std::vector<Block> blocks;
    blocks.reserve(s.blocks);
    for (std::uint32_t b = 0; b < s.blocks; ++b) blocks.emplace_back(storage, b);

    // The plants fire once, on the tile message of rank `target` (counted as published).
    const std::uint64_t total_tiles = static_cast<std::uint64_t>(s.blocks) * s.neighbours * s.nodes;
    const std::uint64_t target      = total_tiles / 2;
    std::uint64_t       published   = 0;
    bool                planted     = false;

    std::deque<Message> pool;
    std::uint64_t       lcg = 0x2545f4914f6cdd1dULL;
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

    auto publish = [&](const Block &from, std::uint32_t g, const std::int64_t *tile) {
        from.for_each_neighbour([&](std::uint32_t to) {
            Message m{Message::kTile, to, g, from.id(), tile};
            if (published++ == target && !planted) {
                planted = true;
                if (mode == "lost") return;
                if (mode == "duplicate") pool.push_back(m);
                if (mode == "far-ahead") m.generation = g + 2;
                if (mode == "misroute") {
                    // The block diagonally across: neither in the sender's row nor its column.
                    const std::uint32_t r = (from.id() / s.grid + 1) % s.grid;
                    const std::uint32_t c = (from.id() % s.grid + 1) % s.grid;
                    m.dst                 = r * s.grid + c;
                }
                if (mode == "misroute-row") {
                    // Another block of the sender's row that is not this tile's destination.
                    const std::uint32_t r = from.id() / s.grid;
                    std::uint32_t       c = (from.id() % s.grid + 1) % s.grid;
                    if (r * s.grid + c == to) c = (c + 1) % s.grid;
                    if (r * s.grid + c != from.id()) m.dst = r * s.grid + c;
                }
            }
            pool.push_back(m);
        });
    };

    auto run_phases = [&](Block &b) {
        do {
            const std::uint32_t own  = b.generation();
            const std::int64_t *tile = b.compute();
            if (mode == "skip-change" && !planted && b.id() == 7 % s.blocks) {
                // The phase ran -- compute() counted it and folded its sum into the witness -- and
                // its result is thrown away: the block keeps its previous tile. Only a phase that
                // changed something is a defect; the first one is taken.
                std::int64_t       *next    = storage.tile(b.id(), (own + 1) & 1u);
                const std::int64_t *prev    = storage.tile(b.id(), own & 1u);
                const std::size_t   n       = static_cast<std::size_t>(s.block) * s.block;
                std::size_t         changed = 0;
                for (std::size_t i = 0; i < n; ++i) changed += next[i] != prev[i];
                if (changed != 0) {
                    planted = true;
                    std::fprintf(stderr,
                                 "apsp subject: block 7's phase %u changed %zu distances; "
                                 "discarded\n",
                                 own + 1, changed);
                    std::memcpy(next, prev, n * sizeof(std::int64_t));
                }
            }
            if (b.publishes()) publish(b, b.generation(), tile);
        } while (b.ready());
    };

    std::uint64_t dones = 0;
    watch.start();
    for (std::uint32_t b = 0; b < s.blocks; ++b) pool.push_back({Message::kInitial, b, 0, 0, nullptr});
    while (!pool.empty()) {
        const Message m = next_message();
        if (m.kind == Message::kDone) {
            if (++dones == s.blocks) {
                watch.stop();
                break;
            }
            continue;
        }
        Block &b = blocks[m.dst];
        if (m.kind == Message::kInitial) {
            publish(b, 0, b.on_initial());
            continue;
        }
        bool complete = b.accept(m.generation, m.from, m.tile);
        if (mode == "early" && !planted && b.id() == 0 && b.generation() == 3
            && storage.state(0).pending[1] == s.neighbours - 1) {
            planted  = true;
            complete = true;  // computes with one tile short: compute() must refuse it
        }
        if (complete) run_phases(b);
        if (b.finished() && complete) pool.push_back({Message::kDone, 0, 0, b.id(), nullptr});
    }
    if (dones != s.blocks) block_forever();

    // What a check WITHOUT the witness would have seen, said on stderr so the battery can show that
    // a plant passes it: the counts against their closed form, the final matrix against the
    // reference (computed here, outside the window).
    const Reference ref                = reference(s);
    bool            counts_as_expected = true;
    bool            matrix_as_expected = true;
    for (std::uint32_t b = 0; b < s.blocks; ++b) {
        const State        &x = storage.state(b);
        const std::int64_t *t = storage.tile(b, x.generation & 1u);
        counts_as_expected    = counts_as_expected && x.generation == s.nodes
                             && x.received == static_cast<std::uint64_t>(s.neighbours) * s.nodes;
        const std::size_t r0 = static_cast<std::size_t>(b / s.grid) * s.block;
        const std::size_t c0 = static_cast<std::size_t>(b % s.grid) * s.block;
        for (std::size_t i = 0; i < s.block; ++i)
            for (std::size_t j = 0; j < s.block; ++j)
                matrix_as_expected = matrix_as_expected
                                     && t[i * s.block + j] == ref.matrix[(r0 + i) * s.nodes + c0 + j];
    }
    std::fprintf(stderr, "apsp subject: every block's phases and tiles as the closed form says: %s\n",
                 counts_as_expected ? "yes" : "no");
    std::fprintf(stderr, "apsp subject: the final matrix as the reference says: %s\n",
                 matrix_as_expected ? "yes" : "no");

    qvo::Answer a{checksum(storage), delivered(storage, dones)};
    a.observed[kObservedAhead] = ahead(storage);
    return a;
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/apsp";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::apsp::params();
    spec.expected          = qvospec::savina::apsp::expected;
    spec.expected_messages = qvospec::savina::apsp::expected_messages;
    spec.work_unit         = qvospec::savina::apsp::kWorkUnit;
    spec.work_units        = qvospec::savina::apsp::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "the block grid simulated on one thread; plants a defect selected by "
                             "QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
