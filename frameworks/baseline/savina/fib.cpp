// @benchmark     savina/fib
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h, with an
//                "actor" that is born and dies inside the window: a heap node in a per-worker
//                slot table, allocated by its parent's worker and freed after it has responded.
// @idiom-note    The floor for creating an actor is what nothing can avoid: one `new`, one slot
//                in a table, one `delete`. Ids are `slot * W + w`, so a child stays on its
//                parent's worker (owner = id % W) and no table is ever touched by two threads --
//                the same static placement qb's addRefActor has, and no balancing, which is
//                what the frameworks with a pool pay for. What a framework adds on top is a
//                mailbox, a registry the id resolves through, and the lifecycle bookkeeping.

#include <qvospec/savina/fib.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_fib_baseline {

using namespace qvospec::savina::fib;

// kRequest: a = n.  kResponse: a = value << 32 | messages, b = chk (Msg has two payload words;
// value < 2^32 for every n below 48 and the message count of a sub-tree is far smaller).
enum Tag : std::uint32_t { kRequest = 1, kResponse = 2 };

struct Node {
    std::uint32_t parent{0};
    std::uint32_t pending{0};
    std::uint64_t value{0};
    std::uint64_t chk{0};
    std::uint64_t messages{0};
};

// One worker's actors. Touched only by the thread owning the worker (and by the main thread
// before start, for the seeds).
struct Table {
    std::vector<Node *>        slots;
    std::vector<std::uint32_t> free_slots;

    std::uint32_t alloc(unsigned w, unsigned W, std::uint32_t parent) {
        std::uint32_t slot;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(slots.size());
            slots.push_back(nullptr);
        }
        slots[slot] = new Node{parent, 0, 0, 0, 0};
        return slot * W + w;
    }

    void release(std::uint32_t slot) {
        delete slots[slot];
        slots[slot] = nullptr;
        free_slots.push_back(slot);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     n     = static_cast<std::uint32_t>(p.get("n"));
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;

    // The root (the driver) is id 0 on worker 0: slot 0 of table 0, never freed.
    std::vector<Table> tables(W);
    const std::uint32_t root = tables[0].alloc(0, W, 0);
    std::uint32_t       seeds[2];
    for (unsigned s = 0; s < 2; ++s) {
        const unsigned w = s % W;
        seeds[s]         = tables[w].alloc(w, W, root);
    }

    std::uint64_t result    = 0;
    std::uint64_t delivered = 0;
    std::uint32_t dones     = 0;

    auto respond = [&](auto &m, unsigned worker, std::uint32_t self, Node &node,
                       std::uint64_t value, std::uint64_t chk) {
        m.send(worker, qvobase::Msg{node.parent, kResponse, (value << 32) | node.messages, chk});
        tables[worker].release(self / W);
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kRequest: {
            Node &node = *tables[worker].slots[msg.dst / W];
            ++node.messages;
            const auto nn = static_cast<std::uint32_t>(msg.a);
            if (nn <= 2) {
                respond(m, worker, msg.dst, node, 1, qvo::mix(1));
                break;
            }
            const std::uint32_t a = tables[worker].alloc(worker, W, msg.dst);
            const std::uint32_t b = tables[worker].alloc(worker, W, msg.dst);
            // Before the sends, and nothing of `node` touched after them: a send to a full
            // ring owned by this worker drains inline, which can run a leaf child's handler
            // AND deliver its response to this node -- and free it -- before send() returns.
            node.pending = 2;
            m.send(worker, qvobase::Msg{a, kRequest, nn - 1, 0});
            m.send(worker, qvobase::Msg{b, kRequest, nn - 2, 0});
            break;
        }
        case kResponse: {
            const std::uint64_t value    = msg.a >> 32;
            const std::uint64_t messages = msg.a & 0xffffffffu;
            if (msg.dst == root) {
                result += msg.b;
                delivered += 1 + messages;
                if (++dones == 2) {
                    watch.stop();
                    m.stop();
                }
                break;
            }
            Node &node = *tables[worker].slots[msg.dst / W];
            node.messages += 1 + messages;
            node.value += value;
            node.chk += msg.b;
            if (--node.pending == 0)
                respond(m, worker, msg.dst, node, node.value, qvo::mix(node.value) + node.chk);
            break;
        }
        }
    });
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{seeds[0], kRequest, n - 1, 0});
    mesh.send(0, qvobase::Msg{seeds[1], kRequest, n - 2, 0});
    mesh.run();

    tables[0].release(0);
    return qvo::Answer{result, delivered};
}

}  // namespace savina_fib_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fib::params();
    spec.expected          = qvospec::savina::fib::expected;
    spec.expected_messages = qvospec::savina::fib::expected_messages;
    spec.work_unit         = qvospec::savina::fib::kWorkUnit;
    spec.work_units        = qvospec::savina::fib::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; an actor is a heap node in its worker's slot table, created "
                             "by its parent's worker and freed after responding; not an actor "
                             "framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. An actor is one `new`, one slot and one `delete`, with no "
        "mailbox, no registry and no lifecycle beyond the slot: the floor for what creating and "
        "destroying an actor can cost",
        "a child is allocated on its parent's worker (id = slot * cores + worker), the same "
        "static placement qb's addRefActor has: with cores=2 the two sub-trees never balance, "
        "so this floor is a bound for the placing frameworks and NOT for the pools",
        "cores=1 is one thread with every node in its own ring -- the floor for single-threaded "
        "dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "with tens of thousands of nodes in flight never happens inside the window"};

    return qvo::run(argc, argv, std::move(spec), savina_fib_baseline::body);
}
