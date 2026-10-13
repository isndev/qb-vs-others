// The negative control's uct subject: savina/uct's verification, watched rejecting each fault its
// answer and its protocol checks exist to see.
//
// savina/uct's answer is the TREE: every node reports its identity, depth, computation size,
// urgency, whether it was granted, the UpdateGrants it received, its busy work and every delivery
// it handled, summed up the tree by a convergecast; the plan the spec computes before the run
// fixes every one of those for every node, whatever the interleaving. This subject runs the
// protocol on one thread -- a FIFO of messages, the root and every node a record -- through the
// spec's own decide(), Tally and checksum(), exactly as the four adapters do, the adapters'
// protocol checks included. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none                correct -- the POSITIVE control: must verify.
//   misroute-grant      the first grant at depth >= 2 goes to a sibling of the node that asked,
//                       one the plan never grants: the tree grows under another identity.
//   wrong-comp          the first grant carries a computation size one too large.
//   lost-update         the first UpdateGrant is lost -- it blocks nothing.
//   duplicate-grant     the first grant is delivered twice.
//   duplicate-request   the first request is delivered twice.
//   duplicate-traverse  the first Traverse that reaches a leaf is delivered twice.
//   skip-work           the first node to traverse skips its busy work (run it with work >= 1).
//   untouched           the first leaf to be traversed reports without its handler running: no
//                       delivery counted, no busy work.
//
// Each plant must be REJECTED by the harness: by the checksum, by the message count, or by the
// protocol check that sees it first (savina/uct: protocol violated -- ...). Run with
// --param nodes=5, the spec must refuse the run before anything is built (exit 2): the cap must
// leave room for the root's first generation.
//
// Like the other subjects, it is NOT built by qvo_add_benchmark and its name does not start with
// "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/uct.h>

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace {

using namespace qvospec::savina::uct;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

enum Kind { kTry, kRequest, kGenerate, kUpdate, kTraverse, kReport };

struct Message {
    Kind          kind{kTry};
    std::uint32_t to{0};  // a node's index; the root is 0
    std::uint64_t a{0};   // kRequest: the asking node; kGenerate: comp; kUpdate: child index
    std::uint64_t b{0};   // kGenerate: urgent child + 1 (0: not urgent)
    Tally         tally{};
};

struct Node {
    std::uint32_t              parent{0};
    std::uint64_t              id{0};
    std::uint32_t              depth{0};
    std::uint32_t              comp{0};
    std::uint32_t              index{0};
    bool                       urgent{false};
    bool                       granted{false};
    bool                       traversed{false};
    std::uint32_t              reports{0};
    std::uint64_t              updated{0};
    Tally                      tally{};
    std::vector<std::uint32_t> children;
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Plan          plan  = qvospec::savina::uct::plan(p, false);
    const std::uint32_t B     = plan.cfg.binomial;
    const std::uint32_t work  = plan.cfg.work;
    const std::string   which = plant();

    std::vector<Node>   nodes(1);  // the root
    std::deque<Message> queue;
    std::uint64_t       requests = 0, grants = 0;
    bool                complete = false, done = false;
    bool                planted  = false;  // each plant fires once

    auto create = [&](std::uint32_t parent, std::uint32_t i, std::uint32_t comp, bool urgent) {
        Node n;
        n.parent = parent;
        n.id     = child_id(nodes[parent].id, i, B);
        n.depth  = nodes[parent].depth + 1;
        n.comp   = comp;
        n.index  = i;
        n.urgent = urgent;
        nodes.push_back(n);
        const auto idx = static_cast<std::uint32_t>(nodes.size() - 1);
        nodes[parent].children.push_back(idx);
        return idx;
    };
    auto report = [&](std::uint32_t n) {
        Node &x = nodes[n];
        x.tally.own(x.id, x.depth, x.comp, x.urgent, x.granted);
        Message m;
        m.kind  = kReport;
        m.to    = x.parent;
        m.tally = x.tally;
        queue.push_back(m);
    };

    watch.start();
    for (std::uint32_t i = 0; i < B; ++i) create(0, i, plan.root_comp, false);
    for (std::uint32_t c : nodes[0].children) queue.push_back(Message{kTry, c});

    while (!queue.empty() && !done) {
        const Message m = queue.front();
        queue.pop_front();
        switch (m.kind) {
        case kTry: {
            Node &n = nodes[m.to];
            ++n.tally.messages;
            n.tally.work += try_work(n.id, work);
            queue.push_back(Message{kRequest, 0, m.to});
            if (which == "duplicate-request" && !planted) {
                planted = true;
                queue.push_back(Message{kRequest, 0, m.to});
            }
            break;
        }
        case kRequest: {
            Node &root = nodes[0];
            ++root.tally.messages;
            if (complete) fail("a request after the tree is complete");
            ++requests;
            const auto  asker = static_cast<std::uint32_t>(m.a);
            const Grant g     = decide(plan, nodes[asker].id, nodes[asker].depth);
            if (g.granted) {
                ++grants;
                Message gm{kGenerate, asker, g.comp, g.urgent ? g.urgent_child + 1u : 0u};
                if (!planted && nodes[asker].depth >= 2) {
                    if (which == "wrong-comp") {
                        planted = true;
                        gm.a += 1;
                    } else if (which == "duplicate-grant") {
                        planted = true;
                        queue.push_back(gm);
                    } else if (which == "misroute-grant") {
                        // a sibling the plan never grants: its coin says leaf
                        for (std::uint32_t s : nodes[nodes[asker].parent].children)
                            if (s != asker && (node_draw(nodes[s].id) & 1u) == 0) {
                                planted = true;
                                gm.to   = s;
                                break;
                            }
                    }
                }
                queue.push_back(gm);
            }
            if (requests != B + B * grants) break;
            complete = true;
            for (std::uint32_t c : root.children) queue.push_back(Message{kTraverse, c});
            break;
        }
        case kGenerate: {
            ++nodes[m.to].tally.messages;
            if (nodes[m.to].granted) fail("a second grant at a node");
            nodes[m.to].granted = true;
            if (!(which == "lost-update" && !planted)) queue.push_back(Message{kUpdate, nodes[m.to].parent, nodes[m.to].index});
            else planted = true;
            for (std::uint32_t i = 0; i < B; ++i)
                create(m.to, i, static_cast<std::uint32_t>(m.a), m.b != 0 && i + 1 == m.b);
            for (std::uint32_t c : nodes[m.to].children) queue.push_back(Message{kTry, c});
            break;
        }
        case kUpdate: {
            Node &n = nodes[m.to];
            ++n.tally.messages;
            const std::uint64_t bit = std::uint64_t{1} << m.a;
            if (n.updated & bit) fail("an UpdateGrant repeated");
            n.updated |= bit;
            n.tally.update_w += child_id(n.id, static_cast<std::uint32_t>(m.a), B);
            break;
        }
        case kTraverse: {
            Node &n = nodes[m.to];
            if (n.children.empty() && !planted && which == "untouched") {
                planted = true;
                report(m.to);
                break;
            }
            ++n.tally.messages;
            if (n.traversed) fail("a node traversed twice");
            n.traversed = true;
            if (which == "skip-work" && !planted) planted = true;
            else n.tally.work += traverse_work(n.id, n.comp, work);
            if (n.children.empty()) {
                if (which == "duplicate-traverse" && !planted) {
                    planted = true;
                    queue.push_back(Message{kTraverse, m.to});
                }
                report(m.to);
                break;
            }
            for (std::uint32_t c : n.children) queue.push_back(Message{kTraverse, c});
            break;
        }
        case kReport: {
            Node &n = nodes[m.to];
            ++n.tally.messages;
            n.tally.add(m.tally);
            if (++n.reports > n.children.size()) fail("a report beyond the node's children");
            if (n.reports != n.children.size()) break;
            if (m.to == 0) {
                watch.stop();
                done = true;
            } else {
                report(m.to);
            }
            break;
        }
        }
    }
    if (!done) watch.stop();  // a plant that starves the protocol: the answer says so
    return qvo::Answer{checksum(nodes[0].tally), nodes[0].tally.messages};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/uct";
    spec.framework         = "control";
    spec.framework_version = "0";
    spec.params            = qvospec::savina::uct::params();
    spec.expected          = qvospec::savina::uct::expected;
    spec.expected_messages = qvospec::savina::uct::expected_messages;
    spec.work_unit         = qvospec::savina::uct::kWorkUnit;
    spec.work_units        = qvospec::savina::uct::work_units;
    spec.idiom_source      = "none -- the negative control's uct subject";
    spec.idiom_note        = "savina/uct's protocol on one thread through the spec's decide(), "
                             "Tally and checksum(), one planted fault at a time (QVO_CONTROL_PLANT)";
    return qvo::run(argc, argv, std::move(spec), body);
}
