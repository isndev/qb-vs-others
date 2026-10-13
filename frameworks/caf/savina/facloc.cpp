// @benchmark     savina/facloc
// @framework     caf 1.1.0
// @idiom-source  the uct and fork-join-create adapters beside this file (function-based
//                behaviors, stateful_actor, built-in atoms, `self->spawn(...)` from inside a
//                behavior for a child created at run time, `self->quit()` for an actor's own end)
//                -- CAF's own idioms from libcaf_core/caf/scheduled_actor.hpp.
// @idiom-note    The reference's actors one for one: the producer spawns the root quadrant, a
//                quadrant that splits spawns its four children with their seeds as spawn
//                arguments, and each quits after its ConfirmExit. Messages are built-in atoms plus
//                arguments: a customer `(put_atom, uint64 x | y << 32, uint32 index)`,
//                NextCustomer `(get_atom)`, Facility `(update_atom, uint64 quadrant, uint32
//                depth)`, RequestExit `(close_atom)` and ConfirmExit `(ok_atom, nine uint64)`, the
//                spec's Tally field by field. The work-stealing pool places every quadrant.

#include <qvospec/savina/facloc.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace savina_facloc_caf {

using namespace qvospec::savina::facloc;

struct Sink {
    Tally         tally;
    std::uint64_t producer_messages{0};
};

template <typename Self>
void mail_confirm(Self *self, const caf::actor &to, const Tally &t) {
    self->mail(caf::ok_atom_v, t.quadrants, t.facilities, t.fac_w, t.customers, t.cust_ids,
               t.cust_w, t.notif_w, t.depth_sum, t.messages)
        .send(to);
}

struct quad_state {
    const Config             *cfg{nullptr};
    caf::actor                parent;    // none for the root
    caf::actor                producer;  // the root's only
    qvo::Watch               *watch{nullptr};
    Sink                     *sink{nullptr};
    std::optional<Quadrant>   q;
    std::array<caf::actor, 4> children;
    std::uint32_t             confirms{0};
    bool                      exiting{false};
    Tally                     tally;
};

caf::behavior quadrant_fun(caf::stateful_actor<quad_state> *self, const Config *cfg, caf::actor parent,
                           Seed seed, caf::actor producer, qvo::Watch *watch, Sink *sink) {
    auto &st    = self->state();
    st.cfg      = cfg;
    st.parent   = std::move(parent);
    st.producer = std::move(producer);
    st.watch    = watch;
    st.sink     = sink;
    st.q.emplace(std::move(seed), cfg->threshold);

    auto split = [self] {
        auto               &s     = self->state();
        std::array<Seed, 4> seeds = s.q->split();
        if (s.parent) self->mail(caf::update_atom_v, s.q->pid(), s.q->depth()).send(s.parent);
        const auto me = caf::actor_cast<caf::actor>(self);
        for (unsigned k = 0; k < 4; ++k)
            s.children[k] = self->spawn<qvocaf::kSpawnOptions>(quadrant_fun, s.cfg, me, std::move(seeds[k]),
                                                               caf::actor{}, nullptr, nullptr);
    };
    // A quadrant's end: its ConfirmExit to its parent -- or, at the root, the end of the run.
    auto confirm = [self] {
        auto &s = self->state();
        s.tally.own(*s.q, !s.parent);
        if (s.parent) {
            mail_confirm(self, s.parent, s.tally);
        } else {
            s.watch->stop();
            s.sink->tally = s.tally;
        }
        self->quit();
    };

    return {
        [self, split](caf::put_atom, std::uint64_t xy, std::uint32_t index) {
            auto &s = self->state();
            ++s.tally.messages;
            if (s.exiting) fail("a customer after RequestExit");
            const Customer c{static_cast<std::uint32_t>(xy), static_cast<std::uint32_t>(xy >> 32), index};
            if (s.q->open()) self->mail(caf::put_atom_v, xy, index).send(s.children[s.q->route(c)]);
            else if (s.q->add(c)) split();
            if (!s.parent) self->mail(caf::get_atom_v).send(s.producer);  // Savina: the next one
        },
        // Savina's notifyParentOfFacility, on up to the root.
        [self](caf::update_atom, std::uint64_t fid, std::uint32_t depth) {
            auto &s = self->state();
            ++s.tally.messages;
            s.tally.notif_w += fid;
            if (s.parent) self->mail(caf::update_atom_v, fid, depth).send(s.parent);
        },
        [self, confirm](caf::close_atom) {
            auto &s = self->state();
            ++s.tally.messages;
            if (s.exiting) fail("a second RequestExit");
            s.exiting = true;
            if (!s.q->open()) return confirm();
            for (const auto &c : s.children) self->mail(caf::close_atom_v).send(c);
        },
        [self, confirm](caf::ok_atom, std::uint64_t quadrants, std::uint64_t facilities,
                        std::uint64_t fac_w, std::uint64_t customers, std::uint64_t cust_ids,
                        std::uint64_t cust_w, std::uint64_t notif_w, std::uint64_t depth_sum,
                        std::uint64_t messages) {
            auto &s = self->state();
            ++s.tally.messages;
            Tally t;
            t.quadrants  = quadrants;
            t.facilities = facilities;
            t.fac_w      = fac_w;
            t.customers  = customers;
            t.cust_ids   = cust_ids;
            t.cust_w     = cust_w;
            t.notif_w    = notif_w;
            t.depth_sum  = depth_sum;
            t.messages   = messages;
            s.tally.add(t);
            if (++s.confirms > 4) fail("a fifth ConfirmExit");
            if (s.confirms == 4) confirm();
        },
    };
}

struct producer_state {
    const std::vector<Customer> *customers{nullptr};
    qvo::Watch                  *watch{nullptr};
    Sink                        *sink{nullptr};
    caf::actor                   root;
    std::size_t                  produced{0};
};

caf::behavior producer_fun(caf::stateful_actor<producer_state> *self, const Config *cfg,
                           const std::vector<Customer> *customers, qvo::Watch *watch, Sink *sink) {
    auto &st     = self->state();
    st.customers = customers;
    st.watch     = watch;
    st.sink      = sink;
    const auto me = caf::actor_cast<caf::actor>(self);
    st.root = self->spawn<qvocaf::kSpawnOptions>(quadrant_fun, cfg, caf::actor{}, root_seed(*cfg), me,
                                                 watch, sink);
    // The window opens on this message: the system is running, the root spawned.
    self->mail(caf::tick_atom_v).send(me);

    auto produce = [self] {
        auto           &s = self->state();
        const Customer &c = (*s.customers)[s.produced++];
        self->mail(caf::put_atom_v, static_cast<std::uint64_t>(c.x) | (static_cast<std::uint64_t>(c.y) << 32),
                   c.index)
            .send(s.root);
    };
    return {
        [self, produce](caf::tick_atom) {
            self->state().watch->start();
            produce();
        },
        [self, produce](caf::get_atom) {
            auto &s = self->state();
            ++s.sink->producer_messages;
            if (s.produced < s.customers->size()) return produce();
            self->mail(caf::close_atom_v).send(s.root);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config                cfg   = config(p);
    const std::vector<Customer> all   = customers(cfg);  // drawn before the window
    const auto                  cores = static_cast<std::size_t>(p.get("cores") < 1 ? 1 : p.get("cores"));
    const bool                  spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config sys_cfg;
        qvocaf::configure(sys_cfg, cores, spin);

        caf::actor_system sys{sys_cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(producer_fun, &cfg, &all, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{checksum(sink.tally), sink.tally.messages + sink.producer_messages};
}

}  // namespace savina_facloc_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::facloc::params();
    spec.expected          = qvospec::savina::facloc::expected;
    spec.expected_messages = qvospec::savina::facloc::expected_messages;
    spec.work_unit         = qvospec::savina::facloc::kWorkUnit;
    spec.work_units        = qvospec::savina::facloc::work_units;
    spec.idiom_source      = "the uct and fork-join-create adapters + self->spawn() from a behavior "
                             "(scheduled_actor.hpp)";
    spec.idiom_note        = "the producer spawns the root quadrant; a quadrant that splits spawns "
                             "its four children with self->spawn(quadrant_fun, seed); customers "
                             "travel down as (put_atom, xy, index); a quadrant mails its subtree's "
                             "tally to its parent and quits; placement left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "a quadrant spawned from a worker is enqueued on THAT worker and stolen from there, so with "
        "cores=2 the quadtree is spread over the workers dynamically; qb's cell keeps each of the "
        "root's subtrees on one core because qb has no cross-core spawn -- see "
        "benchmarks/savina/facloc.md");

    return qvo::run(argc, argv, std::move(spec), savina_facloc_caf::body);
}
