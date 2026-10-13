// @benchmark     savina/apsp
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the blocks
//                are integers owned by worker bi * cores / grid, each a savina::apsp::Block over
//                the spec's Storage.
// @idiom-note    A tile travels as a pointer in the floor's fixed message (generation and block id
//                in one word, the pointer in the other); a block's phase is the spec's compute(),
//                the same object code every framework runs. The placement is qb's: whole
//                block-rows per worker.

#include <qvospec/savina/apsp.h>

#include "../baseline_support.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace savina_apsp_baseline {

using namespace qvospec::savina::apsp;

enum Tag : std::uint32_t { kInitial = 1, kTile = 2, kDone = 3 };

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape    s    = shape(p);
    const bool     spin = p.get("wait") != 0;
    const unsigned W    = s.cores;
    if (s.form != 0)
        qvo::not_applicable("form=1 is qb's `send` (a generation handed to the peer's ring at once "
                            "instead of at the core's next flush); the floor pushes into the ring "
                            "when it sends, so form=0 is already that");
    Storage storage(s);

    // Mesh ids: worker w owns ids w, w + W, w + 2W, ...; block b goes to worker core_of(s, b) and
    // the sink to worker 0.
    std::vector<std::uint32_t> id_of(s.blocks + 1);
    std::vector<std::uint32_t> block_of;
    std::vector<std::uint32_t> next(W, 0);
    auto assign = [&](std::uint32_t worker) { return next[worker]++ * W + worker; };
    for (std::uint32_t b = 0; b < s.blocks; ++b) id_of[b] = assign(core_of(s, b));
    const std::uint32_t sink = assign(0);
    block_of.assign(static_cast<std::size_t>(*std::max_element(next.begin(), next.end())) * W, 0);
    for (std::uint32_t b = 0; b < s.blocks; ++b) block_of[id_of[b]] = b;

    std::vector<Block> blocks;
    blocks.reserve(s.blocks);
    for (std::uint32_t b = 0; b < s.blocks; ++b) blocks.emplace_back(storage, b);
#ifdef QVO_REAL_ADAPTER_CONTROLS
    std::vector<AdapterControl> controls;
    controls.reserve(s.blocks);
    for (std::uint32_t b = 0; b < s.blocks; ++b) controls.emplace_back(storage, b);
#endif
    std::uint64_t dones = 0;  // the Dones the sink's handler ran

    auto handler = [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        if (msg.dst == sink) {
            if (++dones == s.blocks) {
                watch.stop();
                m.stop();
            }
            return;
        }
        Block &b       = blocks[block_of[msg.dst]];
        auto   publish = [&](std::uint32_t g, const std::int64_t *tile) {
            b.for_each_neighbour([&](std::uint32_t to) {
                m.send(worker, qvobase::Msg{id_of[to], kTile,
                                            (static_cast<std::uint64_t>(g) << 32) | b.id(),
                                            reinterpret_cast<std::uintptr_t>(tile)});
            });
        };
        if (msg.tag == kInitial) {
            publish(0, b.on_initial());
            return;
        }
#ifdef QVO_REAL_ADAPTER_CONTROLS
        if (!controls[b.id()].accept(b, static_cast<std::uint32_t>(msg.a >> 32),
                                    static_cast<std::uint32_t>(msg.a),
                                    reinterpret_cast<const std::int64_t *>(static_cast<std::uintptr_t>(msg.b))))
#else
        if (!b.accept(static_cast<std::uint32_t>(msg.a >> 32), static_cast<std::uint32_t>(msg.a),
                      reinterpret_cast<const std::int64_t *>(static_cast<std::uintptr_t>(msg.b))))
#endif
            return;
        do {
#ifdef QVO_REAL_ADAPTER_CONTROLS
            const std::int64_t *tile = controls[b.id()].compute(b);
#else
            const std::int64_t *tile = b.compute();
#endif
            if (b.publishes()) publish(b.generation(), tile);
        } while (b.ready());
        if (b.finished()) m.send(worker, qvobase::Msg{sink, kDone, 0, 0});
    };
    using Mesh = qvobase::Mesh<decltype(handler)>;
    // The Mesh drains a FULL ring of its own worker inline, which would re-enter a block's handler
    // while it publishes. No ring fills when everything that can be in flight at once fits in
    // one: a tile of each of two generations per (block, neighbour), the Initials and the Dones.
    const std::uint64_t in_flight = static_cast<std::uint64_t>(s.blocks) * (2ull * s.neighbours + 2);
    if (in_flight > Mesh::kRingCapacity)
        qvo::not_applicable("the floor's rings hold " + std::to_string(Mesh::kRingCapacity)
                            + " messages and this grid can have " + std::to_string(in_flight)
                            + " in flight; a full ring of the sending worker is drained inline, "
                              "which would re-enter a block's handler");
    Mesh mesh(W, spin, handler);
    mesh.start();

    watch.start();
    for (std::uint32_t b = 0; b < s.blocks; ++b) mesh.send(0, qvobase::Msg{id_of[b], kInitial, 0, 0});
    mesh.run();

    // run() joined every worker: every block's state is final in the Storage, reduced here,
    // outside the window.
#ifdef QVO_REAL_ADAPTER_CONTROLS
    explain_adapter_control(storage);
#endif
    qvo::Answer answer{checksum(storage), delivered(storage, dones)};
    answer.observed[kObservedAhead] = ahead(storage);
    return answer;
}

}  // namespace savina_apsp_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::apsp::params();
    spec.expected          = qvospec::savina::apsp::expected;
    spec.expected_messages = qvospec::savina::apsp::expected_messages;
    spec.work_unit         = qvospec::savina::apsp::kWorkUnit;
    spec.work_units        = qvospec::savina::apsp::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; block-row bi owned by worker bi * cores / grid; a tile travels "
                             "as a pointer; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Blocks are integers and a tile is one ring push and one ring pop "
        "per neighbour; the phase is the spec's compute(), the same as every framework's",
        "block-row bi lives on worker bi * cores / grid -- qb's placement: at cores=2 a block's row "
        "neighbours share its worker and half its column neighbours do not",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_apsp_baseline::body);
}
