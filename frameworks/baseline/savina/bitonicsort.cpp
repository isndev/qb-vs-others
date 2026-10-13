// @benchmark     savina/bitonicsort
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: every actor
//                of the reference's network an integer, its state one slot of a vector, its kind a
//                switch, the logic the spec's Splitter / Comparator / Joiner every framework runs.
// @idiom-note    A message is the floor's fixed four-field Msg (a value: a = the value; a stamped
//                value: b = its order id) pushed into a ring. The network's actors live on qb's
//                placement (bitonicsort.h, placement()): each step whole on one worker, the steps cut
//                into contiguous halves of equal message weight at cores=2, so a value crosses a
//                worker once -- an actor's mesh id is slot * cores + worker, which is how the mesh
//                routes it to that worker.

#include <qvospec/savina/bitonicsort.h>

#include "../baseline_support.h"

#include <optional>
#include <vector>

namespace savina_bitonicsort_baseline {

using namespace qvospec::savina::bitonicsort;

enum Tag : std::uint32_t { kStart = 1, kValue = 2, kData = 3, kExit = 4 };

// At most 1.5 n messages are ever in flight in this network (counted on the reference's FIFO), and
// a worker that finds a ring of its own full drains its inbound rings INLINE (Mesh::send) -- which
// could run a splitter's or a joiner's next message before the current one's output is queued. Up
// to 2^14 values the 65 536-slot rings hold 2.6 times what can be in flight, so no ring fills.
inline constexpr long long kMaxN = 1LL << 14;

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;
    if (p.get("n") > kMaxN)
        qvo::not_applicable("the floor's rings hold 65 536 messages and a worker drains a full one "
                            "of its own inline, which could reorder a splitter's run: n above 2^14 "
                            "is not run on the floor -- see frameworks/baseline/savina/bitonicsort.cpp");

    const Reference                 &ref  = reference(p);
    const Topology                  &t    = ref.net;
    const std::vector<std::uint32_t> core = placement(ref, W);
    const Values                     in   = input(p);
    const auto                       size = static_cast<std::uint32_t>(t.nodes.size());

    // Mesh ids: actor i is slot k of worker core[i], id k * W + core[i].
    std::vector<std::uint32_t> mesh_id(size), node_of;
    {
        std::vector<std::uint32_t> slots(W, 0);
        for (std::uint32_t i = 0; i < size; ++i) mesh_id[i] = slots[core[i]]++ * W + core[i];
        std::uint32_t top = 0;
        for (const std::uint32_t id : mesh_id) top = id > top ? id : top;
        node_of.assign(top + 1, kNone);
        for (std::uint32_t i = 0; i < size; ++i) node_of[mesh_id[i]] = i;
    }
    std::vector<Tally>                 tallies(size);
    std::vector<Splitter>              splitters(size);
    std::vector<Comparator>            comparators(size);
    std::vector<std::optional<Joiner>> joiners(size);
    for (std::uint32_t i = 0; i < size; ++i)
        if (t.nodes[i].kind == Kind::kJoiner) joiners[i].emplace(t.nodes[i]);
    Values out;
    out.reserve(in.size() + 1);
    bool done = false;

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        const std::uint32_t i    = node_of[msg.dst];
        const Node         &node = t.nodes[i];
        Tally              &tl   = tallies[i];
        if (msg.tag == kValue || msg.tag == kData) tl.value(msg.a);
        else tl.signal();
        const bool    exit = msg.tag == kExit;
        auto          send = [&](std::uint32_t to, std::uint32_t tag, std::uint64_t a, std::uint64_t b) {
            m.send(worker, qvobase::Msg{mesh_id[to], tag, a, b});
        };
        switch (node.kind) {
        case Kind::kSource:
            for (const std::uint64_t v : in) send(node.next, kValue, v, 0);
            send(node.next, kExit, 0, 0);
            break;
        case Kind::kForward: send(node.next, msg.tag, msg.a, msg.b); break;
        case Kind::kSplitter:
            if (exit)
                for (std::uint32_t k = 0; k < node.lanes; ++k) send(t.receivers[node.first + k], kExit, 0, 0);
            else
                send(t.receivers[node.first + splitters[i].next(node)], kValue, msg.a, 0);
            break;
        case Kind::kCompare: {
            std::uint64_t a = 0, b = 0;
            if (exit) send(node.next, kExit, 0, 0);
            else if (comparators[i].accept(msg.a, node.up, a, b)) {
                send(node.next, kData, a, node.order);
                send(node.next, kData, b, node.order);
            }
            break;
        }
        case Kind::kJoiner:
            if (exit) {
                if (joiners[i]->close()) send(node.next, kExit, 0, 0);
            } else {
                joiners[i]->accept(static_cast<std::uint32_t>(msg.b), msg.a,
                                   [&](std::uint64_t v) { send(node.next, kValue, v, 0); });
            }
            break;
        case Kind::kValueData:
            if (exit) send(node.next, kExit, 0, 0);
            else send(node.next, kData, msg.a, msg.tag == kData ? msg.b : node.order);
            break;
        case Kind::kDataValue:
            send(node.next, exit ? kExit : kValue, msg.a, 0);
            break;
        case Kind::kSink:
            if (!exit) out.push_back(msg.a);
            else if (!done) {  // the first Exit ends the run
                done = true;
                watch.stop();
                m.stop();
            }
            break;
        }
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{mesh_id[t.source()], kStart, 0, 0});
    mesh.run();

    // run() joined every worker: every tally is final, and reduced here, outside the window.
    explain(tallies, out, p);
    return qvo::Answer{checksum(tallies, out), delivered(tallies)};
}

}  // namespace savina_bitonicsort_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bitonicsort::params();
    spec.expected          = qvospec::savina::bitonicsort::expected;
    spec.expected_messages = qvospec::savina::bitonicsort::expected_messages;
    spec.work_unit         = qvospec::savina::bitonicsort::kWorkUnit;
    spec.work_units        = qvospec::savina::bitonicsort::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; every actor of the reference network an integer with its state "
                             "in a vector slot; qb's step placement; not an actor framework and not "
                             "ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Actors are integers, a message is one ring push and one ring "
        "pop, and there is no scheduler: the floor for moving 541 869 small messages (at the "
        "defaults) through a fixed network",
        "each step of the network lives whole on one worker and the steps are cut into contiguous "
        "halves of equal message weight at cores=2 -- the same placement qb gets -- so a value "
        "crosses a worker once",
        "cores=1 is one thread pushing every message into its own ring and popping it back -- the "
        "floor for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_bitonicsort_baseline::body);
}
