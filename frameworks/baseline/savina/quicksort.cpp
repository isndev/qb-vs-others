// @benchmark     savina/quicksort
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h, with the
//                fib floor's "actor" born inside the window -- a heap node in its worker's slot
//                table, allocated by its parent's worker and freed after it has answered.
// @idiom-note    A Sort or a Result moves ownership of ONE heap payload -- the values in a
//                `std::vector<uint64_t>`, and on the way back the sub-tree's counts -- through the
//                ring as a pointer in the message's first word: the elements never move, as in every
//                framework's cell. A child is created on its parent's worker (id = slot * cores +
//                worker), the root's two seeds before the window on worker s % cores: qb's placement
//                and no balancing, so this floor bounds the placing frameworks, not the pools.

#include <qvospec/savina/quicksort.h>

#include "../baseline_support.h"

#include <utility>
#include <vector>

namespace savina_quicksort_baseline {

using namespace qvospec::savina::quicksort;

// kSort: a = Payload*.  kResult: a = Payload*, b = the child's Position.
enum Tag : std::uint32_t { kSort = 1, kResult = 2 };

struct Payload {
    Values data;
    Counts counts;
};

struct Node {
    std::uint32_t parent{0};
    Position      position{Position::kInitial};
    std::uint32_t seeds[2]{0, 0};  // the root's
    Values        held;
    Counts        counts;
    std::uint32_t fragments{0};
};

// One worker's actors, touched only by the thread owning the worker (and by the main thread before
// start, for the root and the seeds).
struct Table {
    std::vector<Node *>        slots;
    std::vector<std::uint32_t> free_slots;

    std::uint32_t alloc(unsigned w, unsigned W, std::uint32_t parent, Position position) {
        std::uint32_t slot;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(slots.size());
            slots.push_back(nullptr);
        }
        slots[slot]           = new Node;
        slots[slot]->parent   = parent;
        slots[slot]->position = position;
        return slot * W + w;
    }

    void release(std::uint32_t slot) {
        delete slots[slot];
        slots[slot] = nullptr;
        free_slots.push_back(slot);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     threshold = static_cast<std::size_t>(p.get("threshold"));
    const auto     cores     = static_cast<unsigned>(p.get("cores"));
    const bool     spin      = p.get("wait") != 0;
    const unsigned W         = cores < 1 ? 1u : cores;

    // The driver is id 0 on worker 0 (slot 0 of table 0, never freed); the root is on worker 0,
    // seed s on worker s % W.
    std::vector<Table>  tables(W);
    const std::uint32_t driver = tables[0].alloc(0, W, 0, Position::kInitial);
    const std::uint32_t root   = tables[0].alloc(0, W, driver, Position::kInitial);
    Node               &root_node = *tables[0].slots[root / W];
    for (unsigned s = 0; s < 2; ++s)
        root_node.seeds[s] = tables[s % W].alloc(s % W, W, root, s == 0 ? Position::kLeft : Position::kRight);

    Values output;
    Counts total;
    auto  *first = new Payload{input(p), {}};

    // Answer the parent with the node's held values and counts, then free the node.
    auto respond = [&](auto &m, unsigned worker, std::uint32_t self, Node &node, Payload *payload) {
        payload->counts = node.counts;
        m.send(worker, qvobase::Msg{node.parent, kResult, reinterpret_cast<std::uint64_t>(payload),
                                    static_cast<std::uint64_t>(node.position)});
        tables[worker].release(self / W);
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        auto *payload = reinterpret_cast<Payload *>(msg.a);
        if (msg.dst == driver) {  // the root's Result: the end of the run
            watch.stop();
            total.add_result(payload->counts);
            output = std::move(payload->data);
            delete payload;
            m.stop();
            return;
        }
        Node &node = *tables[worker].slots[msg.dst / W];
        if (msg.tag == kSort) {
            ++node.counts.sorts;
            if (payload->data.size() < threshold) {
                sort_leaf(payload->data);
                respond(m, worker, msg.dst, node, payload);
                return;
            }
            Split s = split(payload->data);
            delete payload;
            const bool          is_root = node.position == Position::kInitial;
            const std::uint32_t left    = is_root ? node.seeds[0] : tables[worker].alloc(worker, W, msg.dst, Position::kLeft);
            const std::uint32_t right   = is_root ? node.seeds[1] : tables[worker].alloc(worker, W, msg.dst, Position::kRight);
            // Before the sends, and nothing of `node` touched after them: a send to a full ring
            // owned by this worker drains inline, which can run a child AND deliver its Result to
            // this node -- and free it -- before send() returns (the fib floor's rule).
            node.held      = std::move(s.equal);
            node.fragments = 1;
            m.send(worker, qvobase::Msg{left, kSort, reinterpret_cast<std::uint64_t>(new Payload{std::move(s.less), {}}), 0});
            m.send(worker, qvobase::Msg{right, kSort, reinterpret_cast<std::uint64_t>(new Payload{std::move(s.greater), {}}), 0});
            return;
        }
        node.counts.add_result(payload->counts);
        join(node.held, payload->data, static_cast<Position>(msg.b));
        if (++node.fragments == 3) {
            payload->data = std::move(node.held);
            respond(m, worker, msg.dst, node, payload);
            return;
        }
        delete payload;
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{root, kSort, reinterpret_cast<std::uint64_t>(first), 0});
    mesh.run();

    tables[0].release(driver / W);
    // run() joined every worker; reduced here, outside the window.
    const Output o = measure(output);
    explain(o, total, p);
    return qvo::Answer{answer(o, total), total.sorts + total.results};
}

}  // namespace savina_quicksort_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::quicksort::params();
    spec.expected          = qvospec::savina::quicksort::expected;
    spec.expected_messages = qvospec::savina::quicksort::expected_messages;
    spec.work_unit         = qvospec::savina::quicksort::kWorkUnit;
    spec.work_units        = qvospec::savina::quicksort::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; an actor is a heap node in its worker's slot table, a message "
                             "moves one heap payload by pointer; not an actor framework and not "
                             "ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. An actor is one `new`, one slot and one `delete`, a message one "
        "ring push of a pointer to the values it owns: the floor for moving data between actors "
        "that are created per split",
        "a child is allocated on its parent's worker (id = slot * cores + worker) and the root's "
        "two seeds on workers 0 and 1, the same static placement qb's addRefActor has: with "
        "cores=2 each half of the first split is sorted on its own worker and never rebalanced, "
        "so this floor bounds the placing frameworks and NOT the pools",
        "cores=1 is one thread with every node in its own ring -- the floor for single-threaded "
        "dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "at cores=2 happens when one half finishes before the other"};

    return qvo::run(argc, argv, std::move(spec), savina_quicksort_baseline::body);
}
