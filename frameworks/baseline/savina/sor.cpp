// @benchmark     savina/sor
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: every cell,
//                the runner and the peer are integers owned by a worker -- the runner's half of the
//                columns on worker 0, the peer's on worker 1 at cores=2 (qb's placement).
// @idiom-note    A value is the floor's fixed message (generation and cell in one word, the value
//                in the other); a cell is the spec's savina::sor::Cell in a 128-byte slot of its
//                own, written only by its worker. A report carries the cell's index alone: its
//                owner reads the finished cell's counts from the slot, handed over by the ring's
//                release/acquire like the message itself.

#include <qvospec/savina/sor.h>

#include "../baseline_support.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace savina_sor_baseline {

using namespace qvospec::savina::sor;

enum Tag : std::uint32_t { kStart = 1, kValue = 2, kReport = 3, kHalf = 4 };

struct alignas(128) Slot {
    Cell cell;
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape    s    = shape(p);
    const bool     spin = p.get("wait") != 0;
    const unsigned W    = s.cores;

    const std::vector<std::int64_t> start = initial(s);
    std::vector<Slot>               slots(s.cells);
    for (std::uint32_t c = 0; c < s.cells; ++c) slots[c].cell = Cell(s, c, start[c]);
    Owner runner(s, true);
    Owner peer(s, false);

    // Mesh ids: worker w owns ids w, w + W, w + 2W, ...
    constexpr std::uint32_t    kRunner = 0xffffffffu, kPeer = 0xfffffffeu;
    std::vector<std::uint32_t> id_of(s.cells);
    std::vector<std::uint32_t> next(W, 0);
    auto assign = [&](std::uint32_t worker) { return next[worker]++ * W + worker; };
    for (std::uint32_t c = 0; c < s.cells; ++c) id_of[c] = assign(core_of(s, c));
    const std::uint32_t runner_id = assign(runner_core(s));
    const std::uint32_t peer_id   = assign(peer_core(s));
    std::vector<std::uint32_t> what(static_cast<std::size_t>(*std::max_element(next.begin(), next.end())) * W, 0);
    for (std::uint32_t c = 0; c < s.cells; ++c) what[id_of[c]] = c;
    what[runner_id] = kRunner;
    what[peer_id]   = kPeer;

    bool half = false;
    auto handler = [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        const std::uint32_t who = what[msg.dst];
        if (who == kRunner) {
            if (msg.tag == kReport) {
                const Cell &c = slots[msg.a].cell;
                runner.record(c.index(), c.value(), c.updates(), c.received(), c.work());
            } else {  // kHalf
                runner.count();
                half = true;
            }
            if (half && runner.complete()) {
                watch.stop();
                m.stop();
            }
            return;
        }
        if (who == kPeer) {
            if (msg.tag == kStart) {
                peer.count();
                for (std::uint32_t c = 0; c < s.cells; ++c)
                    if (!runner_owns(s, c)) m.send(worker, qvobase::Msg{id_of[c], kStart, 0, 0});
            } else {  // kReport
                const Cell &c = slots[msg.a].cell;
                if (peer.record(c.index(), c.value(), c.updates(), c.received(), c.work()))
                    m.send(worker, qvobase::Msg{runner_id, kHalf, 0, 0});
            }
            return;
        }
        Cell &c       = slots[who].cell;
        auto  publish = [&] {
            const std::uint64_t a = (static_cast<std::uint64_t>(c.generation()) << 32) | c.index();
            c.for_each_neighbour([&](std::uint32_t to) {
                m.send(worker, qvobase::Msg{id_of[to], kValue, a, static_cast<std::uint64_t>(c.value())});
            });
        };
        if (msg.tag == kStart) {
            if (c.start()) publish();
        } else if (c.accept(static_cast<std::uint32_t>(msg.a >> 32), static_cast<std::uint32_t>(msg.a),
                            static_cast<std::int64_t>(msg.b))) {
            c.update();
            if (c.publishes()) publish();
        }
        if (c.report_due())
            m.send(worker, qvobase::Msg{runner_owns(s, who) ? runner_id : peer_id, kReport, who, 0});
    };
    using Mesh = qvobase::Mesh<decltype(handler)>;
    // The Mesh drains a FULL ring of its own worker inline, which would re-enter a cell's handler
    // while it publishes. No ring fills when everything that can be in flight at once fits in one:
    // two neighbours exchange strictly in turn, so a value per edge, plus the Starts and the reports.
    const std::uint64_t in_flight = edges(s) + 2ull * s.cells + 4;
    if (in_flight > Mesh::kRingCapacity)
        qvo::not_applicable("the floor's rings hold " + std::to_string(Mesh::kRingCapacity)
                            + " messages and this grid can have " + std::to_string(in_flight)
                            + " in flight; a full ring of the sending worker is drained inline, "
                              "which would re-enter a cell's handler");
    Mesh mesh(W, spin, handler);
    mesh.start();

    watch.start();
    for (std::uint32_t c = 0; c < s.cells; ++c)
        if (runner_owns(s, c)) mesh.send(0, qvobase::Msg{id_of[c], kStart, 0, 0});
    mesh.send(0, qvobase::Msg{peer_id, kStart, 0, 0});
    mesh.run();

    // run() joined every worker: the owners' reports are final, reduced here in cell order,
    // outside the window.
    return qvo::Answer{checksum(s, runner, peer), delivered(s, runner, peer)};
}

}  // namespace savina_sor_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sor::params();
    spec.expected          = qvospec::savina::sor::expected;
    spec.expected_messages = qvospec::savina::sor::expected_messages;
    spec.work_unit         = qvospec::savina::sor::kWorkUnit;
    spec.work_units        = qvospec::savina::sor::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the runner's half of the columns on worker 0, the peer's on "
                             "worker 1; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Cells are integers, a value is one ring push and one ring pop, "
        "and a report carries only the cell's index -- the owner reads the finished cell's slot",
        "the runner's half of the columns lives on worker 0 and the peer's on worker 1 at cores=2 "
        "-- qb's placement: only the values across the middle column cross a core",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_sor_baseline::body);
}
