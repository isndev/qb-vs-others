// @benchmark     savina/uct
// @framework     caf 1.1.0
// @idiom-source  the fib and fork-join-create adapters beside this file (function-based
//                behaviors, stateful_actor, built-in atoms, `self->spawn(...)` from inside a
//                behavior for a child created at run time, `self->quit()` for an actor's own end)
//                -- CAF's own idioms from libcaf_core/caf/scheduled_actor.hpp.
// @idiom-note    The reference's actors one for one. Messages are built-in atoms plus arguments,
//                typed per receiver: TryGenerateChildren `(tick_atom)`, ShouldGenerateChildren
//                `(get_atom, actor node, uint64 id, uint32 depth)` -- the node's handle travels in
//                the request, as Savina's `sender` does --, GenerateChildren `(spawn_atom, uint32
//                comp)`, UrgentGenerateChildren `(spawn_atom, uint32 comp, uint32 urgent_child)`,
//                UpdateGrant `(update_atom, uint32 index)`, Traverse `(forward_atom)` and the
//                report `(ok_atom, ten uint64)`, the spec's Tally field by field. Every node is
//                spawned by its parent and quits after its report; the work-stealing pool places
//                every node, so a subtree is balanced across the workers as it grows.

#include <qvospec/savina/uct.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_uct_caf {

using namespace qvospec::savina::uct;

struct Sink {
    Tally tally;
};

template <typename Self>
void mail_report(Self *self, const caf::actor &to, const Tally &t) {
    self->mail(caf::ok_atom_v, t.nodes, t.id_sum, t.id_sq, t.comp_w, t.depth_sum, t.urgent_w,
               t.grant_w, t.update_w, t.work, t.messages)
        .send(to);
}

inline Tally as_tally(std::uint64_t nodes, std::uint64_t id_sum, std::uint64_t id_sq,
                      std::uint64_t comp_w, std::uint64_t depth_sum, std::uint64_t urgent_w,
                      std::uint64_t grant_w, std::uint64_t update_w, std::uint64_t work,
                      std::uint64_t messages) noexcept {
    Tally t;
    t.nodes     = nodes;
    t.id_sum    = id_sum;
    t.id_sq     = id_sq;
    t.comp_w    = comp_w;
    t.depth_sum = depth_sum;
    t.urgent_w  = urgent_w;
    t.grant_w   = grant_w;
    t.update_w  = update_w;
    t.work      = work;
    t.messages  = messages;
    return t;
}

struct node_state {
    const Plan             *plan{nullptr};
    caf::actor              parent;
    caf::actor              root;
    std::uint64_t           id{0};
    std::uint32_t           depth{0};
    std::uint32_t           comp{0};
    std::uint32_t           index{0};
    bool                    urgent{false};
    bool                    granted{false};
    bool                    traversed{false};
    std::uint32_t           reports{0};
    std::uint64_t           updated{0};
    Tally                   tally;
    std::vector<caf::actor> children;
};

caf::behavior node_fun(caf::stateful_actor<node_state> *self, const Plan *plan, caf::actor parent,
                       caf::actor root, std::uint64_t id, std::uint32_t depth, std::uint32_t comp,
                       std::uint32_t index, bool urgent) {
    auto &st  = self->state();
    st.plan   = plan;
    st.parent = std::move(parent);
    st.root   = std::move(root);
    st.id     = id;
    st.depth  = depth;
    st.comp   = comp;
    st.index  = index;
    st.urgent = urgent;

    auto generate = [self](std::uint32_t comp, bool urgent, std::uint32_t urgent_child) {
        auto &s = self->state();
        if (s.granted) fail("a second grant at a node");
        s.granted = true;
        self->mail(caf::update_atom_v, s.index).send(s.parent);
        const std::uint32_t B  = s.plan->cfg.binomial;
        const auto          me = caf::actor_cast<caf::actor>(self);
        s.children.reserve(B);
        for (std::uint32_t i = 0; i < B; ++i)
            s.children.push_back(self->spawn<qvocaf::kSpawnOptions>(
                node_fun, s.plan, me, s.root, child_id(s.id, i, B), s.depth + 1, comp, i,
                urgent && i == urgent_child));
        for (const auto &c : s.children) self->mail(caf::tick_atom_v).send(c);
    };
    auto finish = [self] {
        auto &s = self->state();
        s.tally.own(s.id, s.depth, s.comp, s.urgent, s.granted);
        mail_report(self, s.parent, s.tally);
        self->quit();
    };

    return {
        [self](caf::tick_atom) {
            auto &s = self->state();
            ++s.tally.messages;
            s.tally.work += try_work(s.id, s.plan->cfg.work);
            self->mail(caf::get_atom_v, caf::actor_cast<caf::actor>(self), s.id, s.depth).send(s.root);
        },
        [self, generate](caf::spawn_atom, std::uint32_t comp) {
            ++self->state().tally.messages;
            generate(comp, false, 0);
        },
        [self, generate](caf::spawn_atom, std::uint32_t comp, std::uint32_t urgent_child) {
            ++self->state().tally.messages;
            generate(comp, true, urgent_child);
        },
        [self](caf::update_atom, std::uint32_t index) {
            auto &s = self->state();
            ++s.tally.messages;
            const std::uint64_t bit = std::uint64_t{1} << index;
            if (s.updated & bit) fail("an UpdateGrant repeated");
            s.updated |= bit;
            s.tally.update_w += child_id(s.id, index, s.plan->cfg.binomial);
        },
        [self, finish](caf::forward_atom) {
            auto &s = self->state();
            ++s.tally.messages;
            if (s.traversed) fail("a node traversed twice");
            s.traversed = true;
            s.tally.work += traverse_work(s.id, s.comp, s.plan->cfg.work);
            if (s.children.empty()) return finish();
            for (const auto &c : s.children) self->mail(caf::forward_atom_v).send(c);
        },
        [self, finish](caf::ok_atom, std::uint64_t nodes, std::uint64_t id_sum, std::uint64_t id_sq,
                       std::uint64_t comp_w, std::uint64_t depth_sum, std::uint64_t urgent_w,
                       std::uint64_t grant_w, std::uint64_t update_w, std::uint64_t work,
                       std::uint64_t messages) {
            auto &s = self->state();
            ++s.tally.messages;
            s.tally.add(as_tally(nodes, id_sum, id_sq, comp_w, depth_sum, urgent_w, grant_w,
                                 update_w, work, messages));
            if (++s.reports > s.children.size()) fail("a report beyond the node's children");
            if (s.reports == s.children.size()) finish();
        },
    };
}

struct root_state {
    const Plan             *plan{nullptr};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
    std::uint64_t           requests{0};
    std::uint64_t           grants{0};
    bool                    complete{false};
    std::uint32_t           reports{0};
    std::uint64_t           updated{0};
    Tally                   tally;
    std::vector<caf::actor> children;
};

caf::behavior root_fun(caf::stateful_actor<root_state> *self, const Plan *plan, qvo::Watch *watch,
                       Sink *sink) {
    auto &st = self->state();
    st.plan  = plan;
    st.watch = watch;
    st.sink  = sink;
    // The window opens on this message: the system is running and the root scheduled.
    self->mail(caf::tick_atom_v).send(caf::actor_cast<caf::actor>(self));

    return {
        // Savina's generateTree.
        [self](caf::tick_atom) {
            auto &s = self->state();
            s.watch->start();
            const std::uint32_t B  = s.plan->cfg.binomial;
            const auto          me = caf::actor_cast<caf::actor>(self);
            for (std::uint32_t i = 0; i < B; ++i)
                s.children.push_back(self->spawn<qvocaf::kSpawnOptions>(
                    node_fun, s.plan, me, me, child_id(0, i, B), 1u, s.plan->root_comp, i, false));
            for (const auto &c : s.children) self->mail(caf::tick_atom_v).send(c);
        },
        [self](caf::get_atom, const caf::actor &node, std::uint64_t id, std::uint32_t depth) {
            auto &s = self->state();
            ++s.tally.messages;
            if (s.complete) fail("a request after the tree is complete");
            ++s.requests;
            const Grant g = decide(*s.plan, id, depth);
            if (g.granted) {
                ++s.grants;
                if (g.urgent) self->mail(caf::spawn_atom_v, g.comp, g.urgent_child).send(node);
                else self->mail(caf::spawn_atom_v, g.comp).send(node);
            }
            const std::uint32_t B = s.plan->cfg.binomial;
            if (s.requests != B + B * s.grants) return;
            s.complete = true;  // every node there is has asked: the tree is complete
            for (const auto &c : s.children) self->mail(caf::forward_atom_v).send(c);
        },
        [self](caf::update_atom, std::uint32_t index) {
            auto &s = self->state();
            ++s.tally.messages;
            const std::uint64_t bit = std::uint64_t{1} << index;
            if (s.updated & bit) fail("an UpdateGrant repeated at the root");
            s.updated |= bit;
            s.tally.update_w += child_id(0, index, s.plan->cfg.binomial);
        },
        [self](caf::ok_atom, std::uint64_t nodes, std::uint64_t id_sum, std::uint64_t id_sq,
               std::uint64_t comp_w, std::uint64_t depth_sum, std::uint64_t urgent_w,
               std::uint64_t grant_w, std::uint64_t update_w, std::uint64_t work,
               std::uint64_t messages) {
            auto &s = self->state();
            ++s.tally.messages;
            s.tally.add(as_tally(nodes, id_sum, id_sq, comp_w, depth_sum, urgent_w, grant_w,
                                 update_w, work, messages));
            if (++s.reports > s.children.size()) fail("a report beyond the root's children");
            if (s.reports != s.children.size()) return;
            s.watch->stop();
            s.sink->tally = s.tally;
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Plan plan  = qvospec::savina::uct::plan(p, false);  // the root's decisions, before the window
    const auto cores = static_cast<std::size_t>(p.get("cores") < 1 ? 1 : p.get("cores"));
    const bool spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(root_fun, &plan, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{checksum(sink.tally), sink.tally.messages};
}

}  // namespace savina_uct_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::uct::params();
    spec.expected          = qvospec::savina::uct::expected;
    spec.expected_messages = qvospec::savina::uct::expected_messages;
    spec.work_unit         = qvospec::savina::uct::kWorkUnit;
    spec.work_units        = qvospec::savina::uct::work_units;
    spec.idiom_source      = "the fib and fork-join-create adapters + self->spawn() from a behavior "
                             "(scheduled_actor.hpp)";
    spec.idiom_note        = "a granted node spawns its children with self->spawn(node_fun) and "
                             "mails each (tick_atom); requests carry the node's handle to the root; "
                             "a node mails its subtree's tally to its parent and quits; placement "
                             "left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "a node spawned from a worker is enqueued on THAT worker and stolen from there, so with "
        "cores=2 the tree's subtrees are balanced dynamically as they grow; qb's cell keeps each of "
        "the root's subtrees on one core because qb has no cross-core spawn -- see "
        "benchmarks/savina/uct.md");

    return qvo::run(argc, argv, std::move(spec), savina_uct_caf::body);
}
