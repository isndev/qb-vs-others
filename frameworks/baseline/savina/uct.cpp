// @benchmark     savina/uct
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h, with the fib
//                and fork-join-create floors' notion of an actor born inside the window: a heap
//                node, allocated by the worker of the node that creates it, freed by its parent
//                once it has reported.
// @idiom-note    The floor for growing this tree is what nothing can avoid: one `new` per node,
//                the protocol's messages as the ring's (destination, tag, two words) -- a node's
//                address in the first word -- and one `delete` per node. A node lives on its
//                parent's worker and the root's child i on worker i % cores, allocated by the root
//                and handed over with its first message: the same static placement qb has, without
//                the planter qb needs to create on another core. A report hands the parent the
//                child's record itself, read and freed by the parent. What a framework adds on top
//                is a mailbox, a registry the handle resolves through, and the lifecycle.

#include <qvospec/savina/uct.h>

#include "../baseline_support.h"

#include <cstdint>
#include <vector>

namespace savina_uct_baseline {

using namespace qvospec::savina::uct;

// The extra word a message needs rides in its destination: dst = worker + cores x extra.
//   kTry       a = node                       kRequest   a = node, b = id, extra = depth
//   kGenerate  a = node, b = comp             kUrgent    a = node, b = comp, extra = urgent child
//   kUpdate    a = parent, extra = index      kTraverse  a = node
//   kReport    a = parent, b = the reporting child (its record: read, then freed)
enum Tag : std::uint32_t { kTry = 1, kRequest, kGenerate, kUrgent, kUpdate, kTraverse, kReport };

struct Node {
    unsigned            worker{0};
    Node               *parent{nullptr};
    std::uint64_t       id{0};
    std::uint32_t       depth{0};
    std::uint32_t       comp{0};
    std::uint32_t       index{0};
    bool                urgent{false};
    bool                granted{false};
    bool                traversed{false};
    std::uint32_t       reports{0};
    std::uint64_t       updated{0};
    Tally               tally;
    std::vector<Node *> children;
};

inline std::uint64_t word(Node *n) noexcept { return reinterpret_cast<std::uintptr_t>(n); }
inline Node *node(std::uint64_t w) noexcept { return reinterpret_cast<Node *>(static_cast<std::uintptr_t>(w)); }

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Plan          plan = qvospec::savina::uct::plan(p, false);  // the root's decisions, before the window
    const unsigned      W    = static_cast<unsigned>(p.get("cores") < 1 ? 1 : p.get("cores"));
    const bool          spin = p.get("wait") != 0;
    const std::uint32_t B    = plan.cfg.binomial;
    const std::uint32_t work = plan.cfg.work;

    Node          root;  // id 0, depth 0, on worker 0; its record is never reported, never freed
    std::uint64_t requests = 0;
    std::uint64_t grants   = 0;
    bool          complete = false;

    auto dst = [W](const Node *n, std::uint32_t extra) {
        return static_cast<std::uint32_t>(n->worker + W * extra);
    };
    // Every handler's last action is a send: a send to this worker's own full ring drains it
    // inline, which can run the report that frees the node the handler is working on.
    auto report = [&](auto &m, unsigned worker, Node *n) {
        n->tally.own(n->id, n->depth, n->comp, n->urgent, n->granted);
        m.send(worker, qvobase::Msg{dst(n->parent, 0), kReport, word(n->parent), word(n)});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kTry: {
            Node *n = node(msg.a);
            ++n->tally.messages;
            n->tally.work += try_work(n->id, work);
            m.send(worker, qvobase::Msg{dst(&root, n->depth), kRequest, msg.a, n->id});
            break;
        }
        case kRequest: {  // at the root
            Node               *n     = node(msg.a);
            const std::uint32_t depth = msg.dst / W;
            ++root.tally.messages;
            if (complete) fail("a request after the tree is complete");
            ++requests;
            const Grant g = decide(plan, msg.b, depth);
            if (g.granted) ++grants;
            const bool now_complete = requests == B + B * grants;
            if (now_complete) complete = true;
            if (g.granted)
                m.send(worker, qvobase::Msg{dst(n, g.urgent ? g.urgent_child : 0),
                                            g.urgent ? kUrgent : kGenerate, msg.a, g.comp});
            if (now_complete)
                for (Node *c : root.children) m.send(worker, qvobase::Msg{dst(c, 0), kTraverse, word(c), 0});
            break;
        }
        case kGenerate:
        case kUrgent: {
            Node *n = node(msg.a);
            ++n->tally.messages;
            if (n->granted) fail("a second grant at a node");
            n->granted                      = true;
            const bool          urgent      = msg.tag == kUrgent;
            const std::uint32_t urgent_child = msg.dst / W;
            n->children.reserve(B);
            for (std::uint32_t i = 0; i < B; ++i) {
                Node *c   = new Node;
                c->worker = n->worker;
                c->parent = n;
                c->id     = child_id(n->id, i, B);
                c->depth  = n->depth + 1;
                c->comp   = static_cast<std::uint32_t>(msg.b);
                c->index  = i;
                c->urgent = urgent && i == urgent_child;
                n->children.push_back(c);
            }
            m.send(worker, qvobase::Msg{dst(n->parent, n->index), kUpdate, word(n->parent), 0});
            Node *const      *kids = n->children.data();
            const std::size_t k    = n->children.size();
            for (std::size_t i = 0; i < k; ++i)
                m.send(worker, qvobase::Msg{dst(kids[i], 0), kTry, word(kids[i]), 0});
            break;
        }
        case kUpdate: {
            Node               *n     = node(msg.a);
            const std::uint32_t index = msg.dst / W;
            ++n->tally.messages;
            const std::uint64_t bit = std::uint64_t{1} << index;
            if (n->updated & bit) fail("an UpdateGrant repeated");
            n->updated |= bit;
            n->tally.update_w += child_id(n->id, index, B);
            break;
        }
        case kTraverse: {
            Node *n = node(msg.a);
            ++n->tally.messages;
            if (n->traversed) fail("a node traversed twice");
            n->traversed = true;
            n->tally.work += traverse_work(n->id, n->comp, work);
            if (n->children.empty()) {
                report(m, worker, n);
                break;
            }
            Node *const      *kids = n->children.data();
            const std::size_t k    = n->children.size();
            for (std::size_t i = 0; i < k; ++i)
                m.send(worker, qvobase::Msg{dst(kids[i], 0), kTraverse, word(kids[i]), 0});
            break;
        }
        case kReport: {
            Node *n = node(msg.a);
            Node *c = node(msg.b);
            ++n->tally.messages;
            n->tally.add(c->tally);
            delete c;
            if (++n->reports > n->children.size()) fail("a report beyond the node's children");
            if (n->reports != n->children.size()) break;
            if (n == &root) {
                watch.stop();
                m.stop();
            } else {
                report(m, worker, n);
            }
            break;
        }
        }
    });
    mesh.start();

    // Savina's generateTree, from worker 0 (the calling thread): the root's children, child i on
    // worker i % cores, each sent TryGenerateChildren.
    watch.start();
    root.children.reserve(B);
    for (std::uint32_t i = 0; i < B; ++i) {
        Node *c   = new Node;
        c->worker = i % W;
        c->parent = &root;
        c->id     = child_id(0, i, B);
        c->depth  = 1;
        c->comp   = plan.root_comp;
        c->index  = i;
        root.children.push_back(c);
    }
    for (Node *c : root.children) mesh.send(0, qvobase::Msg{dst(c, 0), kTry, word(c), 0});
    mesh.run();

    return qvo::Answer{checksum(root.tally), root.tally.messages};
}

}  // namespace savina_uct_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::uct::params();
    spec.expected          = qvospec::savina::uct::expected;
    spec.expected_messages = qvospec::savina::uct::expected_messages;
    spec.work_unit         = qvospec::savina::uct::kWorkUnit;
    spec.work_units        = qvospec::savina::uct::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) pair; "
                             "a node is a heap record on its parent's worker, the root's child i on "
                             "worker i % cores; a report hands the parent the child's record, read "
                             "and freed there; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A node is one `new` and one `delete`, with no mailbox, no "
        "registry and no lifecycle beyond its record: the floor for growing and ending the tree",
        "a node lives on its parent's worker and the root's child i on worker i % cores -- the same "
        "static placement qb has, so with cores=2 this floor bounds the placing frameworks and NOT "
        "the pools, and nothing balances the root's subtrees",
        "cores=1 is one thread with every node's messages in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_uct_baseline::body);
}
