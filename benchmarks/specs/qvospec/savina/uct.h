// savina/uct — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Unbalanced Cobwebbed Tree, `uct` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- UctConfig.java for the parameters, UctAkkaActorBenchmark.scala for the
// semantics. Deviations are recorded in benchmarks/savina/uct.md.

#ifndef QVOSPEC_SAVINA_UCT_H
#define QVOSPEC_SAVINA_UCT_H

#include <qvo/harness.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qvospec::savina::uct {

inline constexpr const char *kId = "savina/uct";

// The shape, Savina's own: a ROOT and a tree of NODE actors that grows while the run lasts. The
// root creates `binomial` children; every node, once created, asks the root -- the "cobweb": every
// node of the tree holds a reference to the root and talks to it -- whether it may have children
// (ShouldGenerateChildren). The root answers some of them (GenerateChildren, or
// UrgentGenerateChildren naming one child that is urgent) and ignores the others; a node told to
// generate tells its parent (UpdateGrant), creates `binomial` children and sends each of them
// TryGenerateChildren, which makes them ask the root in turn. Whether a node has children is a coin
// the root tosses, so the tree is UNBALANCED: half the nodes are leaves, and the subtrees under the
// root's children differ by orders of magnitude. Once the tree is complete the root sends Traverse
// down it: every node does its traversal work and passes Traverse to its children.
//
// What the cell measures is creation ON DEMAND (tens of thousands of actors created inside the
// window, each by a parent the root picked), a funnel into one actor (every node's request lands
// on the root) and two waves over an irregular tree. Where a framework places what it creates --
// and whether it can move it -- is the question the imbalance asks.
//
// THE TREE IS THE SAME IN EVERY RUN. Savina's root draws from one java.util.Random(2) in the
// order the requests ARRIVE, and stops granting when its node count reaches the cap: which nodes
// get children, and the whole shape of the tree, depend on the interleaving. Here every draw is a
// function of the NODE (its identity, below), never of an arrival order, and the cap is applied
// in a fixed order the spec computes before the run (plan()): every interleaving builds the same
// tree, and the answer is a property of that tree (uct.md, "The tree, for every interleaving").
//
// `nodes`    -- MAX_NODES, the cap on the tree's size, the root included. Savina: 200 000. The
//               default HERE is 60 000, a DEVIATION with a recorded reason (uct.md): every node
//               of the tree is alive until the end, and qb 3.2.1's actor id was a 16-bit slot per
//               VirtualCore (65 534 live actors) -- 200 000 nodes did not fit on one core. Since
//               qb 3.3.0 the id holds them (a 24-bit ordinary slot), measured; the cap stays the
//               common cell for every framework and `nodes=200000` is a separate document. With
//               the default the tree has 59 991 nodes: 59 990 created inside the window.
// `binomial` -- BINOMIAL_PARAM, the children of a node that has any. Savina's own default, 10.
// `avg`      -- AVG_COMP_SIZE, the mean computation size a grant hands the children. Savina's 500.
// `stdev`    -- STDEV_COMP_SIZE, its standard deviation. Savina's own default, 100.
// `urgent`   -- URGENT_NODE_PERCENT: a grant is UrgentGenerateChildren when a draw in [0, 100)
//               is at most this. Savina's own default, 50 (so 51 % of grants, as in Savina).
// `work`     -- the busy work, a DEVIATION (uct.md, "The busy work"): Savina's two loops count to
//               100 x 40 000 before a request and to comp x 40 000 in a traversal, into a local
//               nobody reads -- dead code a JIT deletes, and hours per repetition if executed.
//               Here `work=0` (the default) runs no busy work; `work=k` runs qvo::spin_work for
//               100 k iterations before each request and comp k in each traversal, Savina's
//               ratio, folded into the answer.
// `cores`    -- worker budget. The frameworks that place put the root on core 0 and the subtree
//               of the root's child i on core i % cores (uct.md, "How `cores` maps").
// `wait`     -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"nodes", 60000}, {"binomial", 10}, {"avg", 500}, {"stdev", 100},
            {"urgent", 50},   {"work", 0},      {"cores", 2}, {"wait", 1}};
}

struct Config {
    std::uint64_t nodes{0};
    std::uint32_t binomial{0};
    std::int64_t  avg{0};
    std::int64_t  stdev{0};
    std::uint32_t urgent{0};
    std::uint32_t work{0};
};

// The parameters, checked before anything is built; outside them the run is refused with the
// harness's fatal exit 2 (qvo::refuse). `binomial` is at most 64 so that a node can keep one bit
// per child (which of its children told it they were granted), and the cap must leave room for
// the root's first generation, which Savina creates unconditionally.
inline Config config(const qvo::Params &p) {
    const long long nodes    = p.get("nodes");
    const long long binomial = p.get("binomial");
    const long long avg      = p.get("avg");
    const long long stdev    = p.get("stdev");
    const long long urgent   = p.get("urgent");
    const long long work     = p.get("work");
    if (binomial < 1 || binomial > 64 || nodes < 1 + binomial || nodes > (1LL << 31) || avg < 1
        || avg > 1000000 || stdev < 0 || stdev > 1000000 || urgent < 0 || urgent > 100 || work < 0
        || work > 100)
        qvo::refuse("savina/uct: need 1 <= binomial <= 64, 1 + binomial <= nodes <= 2^31, "
                    "1 <= avg <= 10^6, 0 <= stdev <= 10^6, 0 <= urgent <= 100, 0 <= work <= 100 "
                    "(got nodes=" + std::to_string(nodes) + " binomial=" + std::to_string(binomial)
                    + " avg=" + std::to_string(avg) + " stdev=" + std::to_string(stdev)
                    + " urgent=" + std::to_string(urgent) + " work=" + std::to_string(work) + ")");
    Config c;
    c.nodes    = static_cast<std::uint64_t>(nodes);
    c.binomial = static_cast<std::uint32_t>(binomial);
    c.avg      = avg;
    c.stdev    = stdev;
    c.urgent   = static_cast<std::uint32_t>(urgent);
    c.work     = static_cast<std::uint32_t>(work);
    return c;
}

// ---------------------------------------------------------------------------------------------
// A node's identity, and every draw made about it
// ---------------------------------------------------------------------------------------------

// The root is 0 and child i (from 0) of node v is v * binomial + i + 1: the node's position in
// the complete binomial-ary tree, a function of its PATH and of nothing a scheduler decides.
// Savina numbers a node with the root's running count at the time of the grant (`currentId =
// size`), which is an arrival order. plan() refuses a tree deep enough to overflow 64 bits.
inline std::uint64_t child_id(std::uint64_t parent, std::uint32_t i, std::uint32_t binomial) noexcept {
    return parent * binomial + i + 1;
}

inline constexpr std::uint64_t kDrawSalt     = 0x7563742d64726177ULL;  // the root's coin and urgency
inline constexpr std::uint64_t kCompSalt     = 0x7563742d636f6d70ULL;  // a grant's computation size
inline constexpr std::uint64_t kPrioSalt     = 0x7563742d7072696fULL;  // the order at the cap depth
inline constexpr std::uint64_t kTrySalt      = 0x7563742d74727921ULL;  // busy work before a request
inline constexpr std::uint64_t kTraverseSalt = 0x7563742d74726176ULL;  // busy work in a traversal

// The root's draw about the node that asks: bit 0 is Savina's `ran.nextBoolean` (children or
// not), bits 8.. its `ran.nextInt(100)` (urgent when at most `urgent`), bits 32.. its
// `ran.nextInt(BINOMIAL_PARAM)` (which child is the urgent one).
inline std::uint64_t node_draw(std::uint64_t id) noexcept { return qvo::mix(kDrawSalt ^ id); }
inline std::uint64_t prio(std::uint64_t id) noexcept { return qvo::mix(kPrioSalt + id); }

// Savina's getNextNormal(AVG_COMP_SIZE, STDEV_COMP_SIZE): a normal draw, rounded, drawn again
// until it is positive. Here the normal variate is Irwin-Hall's -- twelve uniform 16-bit lanes of
// three mix() words, centred and scaled -- in integer arithmetic: the same mean and deviation,
// bounded at six deviations, and identical on every platform, where java.util.Random's
// nextGaussian goes through log() and sqrt() (a libm's last ulp is not portable).
inline std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {  // b > 0
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
inline std::uint32_t comp_size(std::uint64_t key, std::int64_t avg, std::int64_t stdev) noexcept {
    for (std::uint64_t attempt = 0;; ++attempt) {
        std::int64_t lanes = 0;
        for (std::uint64_t w = 0; w < 3; ++w) {
            std::uint64_t x = qvo::mix(key + 3 * attempt + w);
            for (int l = 0; l < 4; ++l, x >>= 16) lanes += static_cast<std::int64_t>(x & 0xffffu);
        }
        // 2 * lanes - 12 * 65535 has mean 0 and a deviation of 2 * 65536 (to 10^-9).
        const std::int64_t v = avg + floor_div(stdev * (2 * lanes - 12 * 65535) + 65536, 131072);
        if (v > 0) return static_cast<std::uint32_t>(v);
    }
}

// What the root answers a node. `comp` is the computation size the node's children inherit,
// `urgent_child` (when `urgent`) the one of them that is urgent -- Savina's GenerateChildren and
// UrgentGenerateChildren(urgentChildId, ...).
struct Grant {
    bool          granted{false};
    bool          urgent{false};
    std::uint32_t urgent_child{0};
    std::uint32_t comp{0};
};

// ---------------------------------------------------------------------------------------------
// The plan: the cap, applied in a fixed order before the run
// ---------------------------------------------------------------------------------------------

// What a node contributes to the answer, and what a parent folds of its subtree. Every field is
// a plain wrapping sum -- no mix() inside the window (Huly QB-1017); the checksum below mixes the
// totals once the run is over.
struct Tally {
    std::uint64_t nodes{0};      // nodes that reported: every node but the root, once
    std::uint64_t id_sum{0};     // their identities
    std::uint64_t id_sq{0};      // their identities squared
    std::uint64_t comp_w{0};     // computation size x identity: what each node's grant gave it
    std::uint64_t depth_sum{0};  // their depths
    std::uint64_t urgent_w{0};   // identities of the urgent nodes
    std::uint64_t grant_w{0};    // identities of the nodes that were granted children
    std::uint64_t update_w{0};   // identities a parent learnt through UpdateGrant
    std::uint64_t work{0};       // the busy work's results (0 at work=0)
    std::uint64_t messages{0};   // deliveries counted at the receivers

    void add(const Tally &o) noexcept {
        nodes += o.nodes;
        id_sum += o.id_sum;
        id_sq += o.id_sq;
        comp_w += o.comp_w;
        depth_sum += o.depth_sum;
        urgent_w += o.urgent_w;
        grant_w += o.grant_w;
        update_w += o.update_w;
        work += o.work;
        messages += o.messages;
    }

    // What node `id` says about itself when it reports.
    void own(std::uint64_t id, std::uint32_t depth, std::uint32_t comp, bool urgent,
             bool granted) noexcept {
        nodes += 1;
        id_sum += id;
        id_sq += id * id;
        comp_w += static_cast<std::uint64_t>(comp) * id;
        depth_sum += depth;
        if (urgent) urgent_w += id;
        if (granted) grant_w += id;
    }
};

// The busy work of a node, both halves, when `work` > 0: the request's (Savina's
// loop(100, dummy)) and the traversal's (loop(comp, dummy)), each seeded by the node's identity.
inline std::uint64_t try_work(std::uint64_t id, std::uint32_t work) noexcept {
    return work == 0 ? 0 : qvo::spin_work(qvo::mix(kTrySalt + id), static_cast<int>(100 * work));
}
inline std::uint64_t traverse_work(std::uint64_t id, std::uint32_t comp, std::uint32_t work) noexcept {
    return work == 0 ? 0
                     : qvo::spin_work(qvo::mix(kTraverseSalt + id),
                                      static_cast<int>(static_cast<std::uint64_t>(comp) * work));
}

// The cap, deterministically. Savina's root grants while `size + BINOMIAL_PARAM <= MAX_NODES`,
// size counting the root and every node created, in whatever order the requests arrive -- which
// in practice is level after level, since each level waits for the round trip of the one above.
// The plan applies the same cap in a FIXED order: level by level from the root, every node whose
// coin says "children" is granted while the cap allows a whole level; at the first level that
// does not fit (the CAP DEPTH), the nodes whose coin says children are granted in the order of a
// per-node priority -- prio(id), the identity breaking a tie -- until the cap is reached; nobody
// deeper is granted. So the tree has exactly the size Savina's has, 1 + B + B g with g the largest
// number of grants the cap allows (59 991 at the defaults), whenever the coins allow that many,
// and its shape is the same for every interleaving.
struct Plan {
    Config        cfg{};
    std::uint32_t root_comp{0};      // the computation size of the root's own children
    std::uint32_t cap_depth{(std::numeric_limits<std::uint32_t>::max)()};  // none: the coins ended it
    bool          rationed{false};   // at cap_depth, grant (prio, id) <= (tau_prio, tau_id)
    std::uint64_t tau_prio{0};
    std::uint64_t tau_id{0};
    std::uint64_t grants{0};         // g: GenerateChildren + UrgentGenerateChildren
    std::uint64_t nodes{0};          // nodes created inside the window: every node but the root
    std::uint32_t height{0};         // the deepest node's depth (the root is 0)
    std::vector<std::uint64_t> first_level;  // the subtree size under each of the root's children
    Tally         expected{};        // what the root holds at the end, `messages` included
};

// The root's answer to node `id` at `depth` -- the function every adapter's root calls, and the
// one plan() itself grants by: one mix() for the coin (Savina's draw per request), three more and
// the priority's for a grant.
inline Grant decide(const Plan &plan, std::uint64_t id, std::uint32_t depth) noexcept {
    Grant               g;
    const std::uint64_t h = node_draw(id);
    if ((h & 1u) == 0 || depth > plan.cap_depth) return g;
    if (depth == plan.cap_depth) {
        if (!plan.rationed) return g;
        const std::uint64_t pr = prio(id);
        if (pr > plan.tau_prio || (pr == plan.tau_prio && id > plan.tau_id)) return g;
    }
    g.granted      = true;
    g.comp         = comp_size(qvo::mix(kCompSalt + id), plan.cfg.avg, plan.cfg.stdev);
    g.urgent       = (h >> 8) % 100 <= plan.cfg.urgent;
    g.urgent_child = static_cast<std::uint32_t>((h >> 32) % plan.cfg.binomial);
    return g;
}

// `with_work`: also run every node's busy work, for the expected answer; an adapter's root needs
// the plan's decisions only and passes false.
inline Plan plan(const qvo::Params &p, bool with_work = true) {
    Plan pl;
    pl.cfg               = config(p);
    const Config      &c = pl.cfg;
    const std::uint32_t B = c.binomial;
    pl.root_comp         = comp_size(qvo::mix(kCompSalt), c.avg, c.stdev);  // the root is node 0
    pl.first_level.assign(B, 0);

    struct Node {
        std::uint64_t id;
        std::uint32_t comp;
        bool          urgent;
        std::uint32_t top;  // which of the root's children this node descends from
    };
    std::vector<Node> level;
    level.reserve(B);
    for (std::uint32_t i = 0; i < B; ++i) level.push_back({child_id(0, i, B), pl.root_comp, false, i});
    std::uint64_t size = 1 + B;  // Savina's `size` after generateTree
    Tally        &t    = pl.expected;
    bool          capped = false;

    for (std::uint32_t depth = 1; !level.empty(); ++depth) {
        pl.height = depth;
        // Which nodes of this level are granted: every node whose coin says so while a whole
        // level fits under the cap; at the cap depth the first ones by (prio, id); below it none.
        std::vector<std::size_t> want;
        if (!capped)
            for (std::size_t n = 0; n < level.size(); ++n)
                if (node_draw(level[n].id) & 1u) want.push_back(n);
        std::vector<char> granted(level.size(), 0);
        if (!capped) {
            const std::uint64_t room = (c.nodes - size) / B;  // grants the cap still allows
            if (want.size() <= room) {
                for (std::size_t n : want) granted[n] = 1;
            } else {
                capped       = true;
                pl.cap_depth = depth;
                pl.rationed  = room > 0;
                std::vector<std::pair<std::uint64_t, std::uint64_t>> order;
                order.reserve(want.size());
                for (std::size_t n : want) order.emplace_back(prio(level[n].id), level[n].id);
                std::sort(order.begin(), order.end());
                if (room > 0) {
                    pl.tau_prio = order[room - 1].first;
                    pl.tau_id   = order[room - 1].second;
                }
                for (std::size_t n : want) {
                    const std::pair<std::uint64_t, std::uint64_t> key{prio(level[n].id), level[n].id};
                    if (room > 0 && key <= order[room - 1]) granted[n] = 1;
                }
            }
        }

        std::vector<Node> next;
        for (std::size_t n = 0; n < level.size(); ++n) {
            const Node &node = level[n];
            const Grant g    = granted[n] ? decide(pl, node.id, depth) : Grant{};
            if (granted[n] != static_cast<char>(g.granted))
                qvo::refuse("savina/uct: the plan disagrees with decide() about node "
                            + std::to_string(node.id));
            t.own(node.id, depth, node.comp, node.urgent, g.granted);
            ++pl.nodes;
            ++pl.first_level[node.top];
            if (with_work)
                t.work += try_work(node.id, c.work) + traverse_work(node.id, node.comp, c.work);
            if (!g.granted) continue;
            if (node.id > ((std::numeric_limits<std::uint64_t>::max)() - B) / B)
                qvo::refuse("savina/uct: the tree's identities would overflow 64 bits below depth "
                            + std::to_string(depth) + " -- a smaller binomial or cap");
            ++pl.grants;
            t.update_w += node.id;  // the granted node's UpdateGrant, at its parent
            size += B;
            for (std::uint32_t i = 0; i < B; ++i)
                next.push_back({child_id(node.id, i, B), g.comp, g.urgent && i == g.urgent_child,
                                node.top});
        }
        level = std::move(next);
    }

    // Every delivery, counted at its receiver: one TryGenerateChildren, one request at the root,
    // one Traverse and one report at the parent per node created; one grant at the node and one
    // UpdateGrant at its parent per grant.
    t.messages = 4 * pl.nodes + 2 * pl.grants;
    return pl;
}

// The checksum of a tally: each field mixed under its own salt, summed. Computed after the
// window, from the root's total; expected() computes it from the plan.
inline std::uint64_t checksum(const Tally &t) noexcept {
    const std::uint64_t field[] = {t.nodes,    t.id_sum,   t.id_sq,    t.comp_w,
                                   t.depth_sum, t.urgent_w, t.grant_w, t.update_w, t.work};
    std::uint64_t       acc     = 0;
    for (std::size_t j = 0; j < std::size(field); ++j)
        acc += qvo::mix(field[j] ^ (0x7563742d73756d00ULL + j * 0x9e3779b97f4a7c15ULL));
    return acc;
}

// THE VERIFIED ANSWER. Every node reports once, to its parent, after its traversal and its
// children's reports -- a convergecast that replaces Savina's Terminate wave (uct.md) -- with
// what it is (its identity, depth, computation size, urgency, whether it was granted), what it
// learnt (the UpdateGrants of its children), its busy work and every delivery it handled, added
// to its subtree's reports. The root's total is the tree, node by node:
//
//   * a node created twice, or a grant delivered twice, adds a node or a subtree; a node never
//     created, or a request, a grant, a Traverse or a report LOST, leaves the root or a parent
//     waiting -- the run blocks and fails at the harness's deadline;
//   * a grant delivered to the wrong node grows children under another identity (id_sum, id_sq);
//     a grant carrying another computation size or urgency moves comp_w or urgent_w;
//   * an UpdateGrant lost or misrouted -- which blocks nothing -- moves update_w against grant_w;
//   * a traversal that skipped its busy work misses its result (work > 0);
//   * a duplicate the protocol can see is a failure at once (qvo::fail through fail() below): a
//     request after the tree is complete, a second grant at a node, a second Traverse, a report
//     beyond `binomial`, an UpdateGrant repeated.
//
// So the answer is the tree -- its node count and its per-node visits -- and is the same for every
// interleaving: the plan fixed every decision before the run.
inline std::uint64_t expected(const qvo::Params &p) { return checksum(plan(p).expected); }

// A protocol violation an adapter sees before any checksum could: qvo::fail() writes the failure
// document and exits 1, from the thread that saw it (never std::abort -- harness.h).
[[noreturn]] inline void fail(const char *what) {
    qvo::fail(std::string("savina/uct: protocol violated -- ") + what);
}

// The report divides by this: one node -- created, asked the root, answered or not, traversed,
// reported, ended.
inline constexpr const char *kWorkUnit = "node";
inline std::uint64_t work_units(const qvo::Params &p) { return plan(p, false).nodes; }

// 4 per node and 2 per grant (plan(), above): 239 960 + 11 996 at the defaults. The root's
// window opens on its own and the adapters' placement messages are not the protocol's: neither is
// counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return plan(p, false).expected.messages;
}

}  // namespace qvospec::savina::uct

#endif  // QVOSPEC_SAVINA_UCT_H
