// @benchmark     savina/facloc
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h, with the uct
//                floor's notion of an actor born inside the window: a heap record holding the
//                spec's quadrant kernel, allocated by the worker of the quadrant that splits,
//                freed by its parent once it has confirmed.
// @idiom-note    The floor for this quadtree is what nothing can avoid: one `new` per quadrant,
//                the protocol's messages as the ring's (destination, tag, two words) -- the
//                quadrant's address in the first word, a customer's coordinates in the second and
//                its index in the destination's spare bits -- and one `delete` per quadrant. A
//                quadrant lives on its parent's worker and the root's child k on worker k % cores,
//                the producer and the root on worker 0: the same static placement qb has, without
//                the planter qb needs to create on another core. A ConfirmExit hands the parent
//                the child's record itself, read and freed by the parent.

#include <qvospec/savina/facloc.h>

#include "../baseline_support.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace savina_facloc_baseline {

using namespace qvospec::savina::facloc;

// The extra word a message needs rides in its destination: dst = worker + cores x extra.
//   kCustomer  a = quadrant, b = x | y << 32, extra = index     kNext     (to the producer)
//   kFacility  a = parent, b = facility's quadrant identity      kExit     a = quadrant
//   kConfirm   a = parent, b = the confirming child (its record: read, then freed)
enum Tag : std::uint32_t { kCustomer = 1, kNext, kFacility, kExit, kConfirm };

struct Quad {
    unsigned                worker{0};
    Quad                   *parent{nullptr};
    std::optional<Quadrant> q;
    std::array<Quad *, 4>   children{};
    std::uint32_t           confirms{0};
    bool                    exiting{false};
    Tally                   tally;
};

inline std::uint64_t word(Quad *q) noexcept { return reinterpret_cast<std::uintptr_t>(q); }
inline Quad *quad(std::uint64_t w) noexcept { return reinterpret_cast<Quad *>(static_cast<std::uintptr_t>(w)); }

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config                cfg  = config(p);
    const std::vector<Customer> all  = customers(cfg);  // drawn before the window
    const unsigned              W    = static_cast<unsigned>(p.get("cores") < 1 ? 1 : p.get("cores"));
    const bool                  spin = p.get("wait") != 0;

    Quad root;  // on worker 0, beside the producer; its record is never freed by a parent
    root.q.emplace(root_seed(cfg), cfg.threshold);
    std::size_t   produced          = 0;
    std::uint64_t producer_messages = 0;

    auto dst = [W](const Quad *q, std::uint64_t extra) {
        return static_cast<std::uint32_t>(q->worker + W * extra);
    };
    auto send_customer = [&](auto &m, unsigned worker, Quad *to, const Customer &c) {
        m.send(worker, qvobase::Msg{dst(to, c.index), kCustomer, word(to),
                                    static_cast<std::uint64_t>(c.x) | (static_cast<std::uint64_t>(c.y) << 32)});
    };
    // A quadrant's end: its ConfirmExit -- the handler's last action, since a send to this
    // worker's own full ring drains it inline, which can run the confirm that frees `n`.
    auto confirm = [&](auto &m, unsigned worker, Quad *n) {
        n->tally.own(*n->q, false);
        m.send(worker, qvobase::Msg{dst(n->parent, 0), kConfirm, word(n->parent), word(n)});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kCustomer: {
            Quad          *n = quad(msg.a);
            const Customer c{static_cast<std::uint32_t>(msg.b), static_cast<std::uint32_t>(msg.b >> 32),
                             msg.dst / W};
            ++n->tally.messages;
            if (n->exiting) fail("a customer after RequestExit");
            if (n->q->open()) {
                send_customer(m, worker, n->children[n->q->route(c)], c);
            } else if (n->q->add(c)) {
                std::array<Seed, 4> seeds = n->q->split();
                for (unsigned k = 0; k < 4; ++k) {
                    Quad *child   = new Quad;
                    child->worker = n == &root ? k % W : n->worker;
                    child->parent = n;
                    child->q.emplace(std::move(seeds[k]), cfg.threshold);
                    n->children[k] = child;
                }
                if (n != &root)
                    m.send(worker, qvobase::Msg{dst(n->parent, 0), kFacility, word(n->parent), n->q->pid()});
            }
            if (n == &root) m.send(worker, qvobase::Msg{0, kNext, 0, 0});  // Savina: the next one
            break;
        }
        case kNext: {
            ++producer_messages;
            if (produced < all.size()) send_customer(m, worker, &root, all[produced++]);
            else m.send(worker, qvobase::Msg{dst(&root, 0), kExit, word(&root), 0});
            break;
        }
        case kFacility: {  // Savina's notifyParentOfFacility, on up to the root
            Quad *n = quad(msg.a);
            ++n->tally.messages;
            n->tally.notif_w += msg.b;
            if (n != &root) m.send(worker, qvobase::Msg{dst(n->parent, 0), kFacility, word(n->parent), msg.b});
            break;
        }
        case kExit: {
            Quad *n = quad(msg.a);
            ++n->tally.messages;
            if (n->exiting) fail("a second RequestExit");
            n->exiting = true;
            if (n->q->open()) {
                const std::array<Quad *, 4> kids = n->children;
                for (Quad *c : kids) m.send(worker, qvobase::Msg{dst(c, 0), kExit, word(c), 0});
            } else if (n == &root) {
                root.tally.own(*root.q, true);
                watch.stop();
                m.stop();
            } else {
                confirm(m, worker, n);
            }
            break;
        }
        case kConfirm: {
            Quad *n = quad(msg.a);
            Quad *c = quad(msg.b);
            ++n->tally.messages;
            n->tally.add(c->tally);
            delete c;
            if (++n->confirms > 4) fail("a fifth ConfirmExit");
            if (n->confirms != 4) break;
            if (n == &root) {
                root.tally.own(*root.q, true);
                watch.stop();
                m.stop();
            } else {
                confirm(m, worker, n);
            }
            break;
        }
        }
    });
    mesh.start();

    // The producer is worker 0 (the calling thread): its first customer opens the window.
    watch.start();
    mesh.send(0, qvobase::Msg{dst(&root, all[0].index), kCustomer, word(&root),
                              static_cast<std::uint64_t>(all[0].x) | (static_cast<std::uint64_t>(all[0].y) << 32)});
    produced = 1;
    mesh.run();

    return qvo::Answer{checksum(root.tally), root.tally.messages + producer_messages};
}

}  // namespace savina_facloc_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::facloc::params();
    spec.expected          = qvospec::savina::facloc::expected;
    spec.expected_messages = qvospec::savina::facloc::expected_messages;
    spec.work_unit         = qvospec::savina::facloc::kWorkUnit;
    spec.work_units        = qvospec::savina::facloc::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) pair; "
                             "a quadrant is a heap record holding the spec's kernel, on its parent's "
                             "worker, the root's child k on worker k % cores; a ConfirmExit hands the "
                             "parent the child's record; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A quadrant is one `new` and one `delete` around the spec's "
        "kernel, with no mailbox, no registry and no lifecycle beyond its record: the floor for "
        "routing the customers and growing the quadtree",
        "the producer and the root on worker 0, a quadrant on its parent's worker and the root's "
        "child k on worker k % cores -- the same static placement qb has, so with cores=2 this "
        "floor bounds the placing frameworks and NOT the pools",
        "cores=1 is one thread with every quadrant's messages in its own ring -- the floor for "
        "single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_facloc_baseline::body);
}
