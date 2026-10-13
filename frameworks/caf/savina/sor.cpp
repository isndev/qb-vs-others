// @benchmark     savina/sor
// @framework     caf 1.1.0
// @idiom-source  the thread-ring and nqueens adapters beside this file (function-based behaviors,
//                stateful_actor, handles wired before the window, built-in atoms and integer
//                message elements -- no custom type).
// @idiom-note    One actor per cell, Savina's runner and peer as the two owners. A cell learns its
//                neighbours and its owner once, from the runner, before the window (Savina passes
//                the actor array in SorStartMessage). A value is `(update_atom, generation, cell,
//                value)`, ONE message per update shared by every neighbour it is mailed to; Start
//                is `tick_atom`, a report `(add_atom, cell, value, updates, received, witness)`,
//                the peer's completed half `join_atom`. Where a cell RUNS is the work-stealing
//                pool's decision.

#include <qvospec/savina/sor.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/message.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_sor_caf {

using namespace qvospec::savina::sor;

struct cell_state {
    Cell                    cell;
    std::vector<caf::actor> neighbours;  // in for_each_neighbour's order
    caf::actor              owner;
};

void publish(caf::stateful_actor<cell_state> *self) {
    auto      &st  = self->state();
    const auto msg = caf::make_message(caf::update_atom_v, st.cell.generation(), st.cell.index(),
                                       st.cell.value());
    for (const auto &n : st.neighbours) self->mail(msg).send(n);
}

void report_if_due(caf::stateful_actor<cell_state> *self) {
    auto &st = self->state();
    if (st.cell.report_due())
        self->mail(caf::add_atom_v, st.cell.index(), st.cell.value(), st.cell.updates(),
                   st.cell.received(), st.cell.work())
            .send(st.owner);
}

caf::behavior cell_fun(caf::stateful_actor<cell_state> *self, const Shape *shape,
                       std::uint32_t index, std::int64_t value) {
    self->state().cell = Cell(*shape, index, value);
    return {
        // Wiring, once per cell, outside the window.
        [self](caf::put_atom, std::vector<caf::actor> neighbours, caf::actor owner,
               caf::actor runner) {
            auto &st      = self->state();
            st.neighbours = std::move(neighbours);
            st.owner      = std::move(owner);
            self->mail(caf::ok_atom_v).send(runner);
        },
        [self](caf::tick_atom) {
            if (self->state().cell.start()) publish(self);
            report_if_due(self);
        },
        [self](caf::update_atom, std::uint32_t generation, std::uint32_t from, std::int64_t v) {
            Cell &c = self->state().cell;
            if (c.accept(generation, from, v)) {
                c.update();
                if (c.publishes()) publish(self);
            }
            report_if_due(self);
        },
    };
}

struct owner_state {
    Owner                  *owner{nullptr};
    std::vector<caf::actor> cells;  // the cells this owner starts
    caf::actor              runner;
};

// Savina's SorPeer: starts its half, collects its reports, reports the half to the runner.
caf::behavior peer_fun(caf::stateful_actor<owner_state> *self, Owner *owner,
                       std::vector<caf::actor> cells) {
    self->state().owner = owner;
    self->state().cells = std::move(cells);
    return {
        [self](caf::put_atom, caf::actor runner) {
            self->state().runner = runner;
            self->mail(caf::ok_atom_v).send(runner);
        },
        [self](caf::tick_atom) {
            auto &st = self->state();
            st.owner->count();
            for (const auto &c : st.cells) self->mail(caf::tick_atom_v).send(c);
        },
        [self](caf::add_atom, std::uint32_t cell, std::int64_t value, std::uint32_t updates,
               std::uint64_t received, std::uint64_t work) {
            auto &st = self->state();
            if (st.owner->record(cell, value, updates, received, work))
                self->mail(caf::join_atom_v).send(st.runner);
        },
    };
}

struct runner_state {
    Owner                  *owner{nullptr};
    std::vector<caf::actor> all;   // every cell, by index
    std::vector<caf::actor> mine;  // the cells the runner starts
    caf::actor              peer;
    std::size_t             ready{0};
    bool                    half{false};
    qvo::Watch             *watch{nullptr};
};

void finish_if_done(caf::stateful_actor<runner_state> *self) {
    auto &st = self->state();
    if (!st.half || !st.owner->complete()) return;
    st.watch->stop();
    for (const auto &c : st.all) self->send_exit(c, caf::exit_reason::user_shutdown);
    self->send_exit(st.peer, caf::exit_reason::user_shutdown);
    self->quit();
}

caf::behavior runner_fun(caf::stateful_actor<runner_state> *self, const Shape *shape,
                         Owner *owner, std::vector<caf::actor> all, caf::actor peer,
                         qvo::Watch *watch) {
    auto &st = self->state();
    st.owner = owner;
    st.all   = std::move(all);
    st.peer  = std::move(peer);
    st.watch = watch;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (std::uint32_t c = 0; c < shape->cells; ++c) {
        const Cell              cell(*shape, c, 0);
        std::vector<caf::actor> neighbours;
        neighbours.reserve(cell.degree());
        cell.for_each_neighbour([&](std::uint32_t n) { neighbours.push_back(st.all[n]); });
        const bool mine = runner_owns(*shape, c);
        if (mine) st.mine.push_back(st.all[c]);
        self->mail(caf::put_atom_v, std::move(neighbours), mine ? me : st.peer, me).send(st.all[c]);
    }
    self->mail(caf::put_atom_v, me).send(st.peer);

    return {
        // Every cell and the peer have been scheduled at least once and are wired: the window
        // that opens here contains the relaxation and not first-touch scheduling.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.all.size() + 1) return;
            s.watch->start();
            for (const auto &c : s.mine) self->mail(caf::tick_atom_v).send(c);
            self->mail(caf::tick_atom_v).send(s.peer);
        },
        [self](caf::add_atom, std::uint32_t cell, std::int64_t value, std::uint32_t updates,
               std::uint64_t received, std::uint64_t work) {
            self->state().owner->record(cell, value, updates, received, work);
            finish_if_done(self);
        },
        [self](caf::join_atom) {
            auto &s = self->state();
            s.owner->count();
            s.half = true;
            finish_if_done(self);
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape s     = shape(p);
    const auto  cores = static_cast<std::size_t>(s.cores);
    const bool  spin  = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    const std::vector<std::int64_t> start = initial(s);
    Owner                           runner(s, true);
    Owner                           peer(s, false);
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> cells;
        std::vector<caf::actor> peer_cells;
        cells.reserve(s.cells);
        for (std::uint32_t c = 0; c < s.cells; ++c) {
            cells.push_back(sys.spawn<qvocaf::kSpawnOptions>(cell_fun, &s, c, start[c]));
            if (!runner_owns(s, c)) peer_cells.push_back(cells.back());
        }
        auto pr = sys.spawn<qvocaf::kSpawnOptions>(peer_fun, &peer, std::move(peer_cells));
        sys.spawn<qvocaf::kSpawnOptions>(runner_fun, &s, &runner, std::move(cells), pr, &watch);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: the owners' reports are final, reduced here in
    // cell order, outside the window.
    return qvo::Answer{checksum(s, runner, peer), delivered(s, runner, peer)};
}

}  // namespace savina_sor_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sor::params();
    spec.expected          = qvospec::savina::sor::expected;
    spec.expected_messages = qvospec::savina::sor::expected_messages;
    spec.work_unit         = qvospec::savina::sor::kWorkUnit;
    spec.work_units        = qvospec::savina::sor::work_units;
    spec.idiom_source      = "the thread-ring and nqueens adapters (built-in atoms, integer "
                             "message elements)";
    spec.idiom_note        = "one actor per cell, Savina's runner and peer as owners; neighbours "
                             "wired once from the runner; one value message per update shared by "
                             "its neighbours; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the cells are placed by the work-stealing pool: a cell made ready by a value stays on the "
        "sender's worker (worker::delay) unless the idle worker steals it, so which values cross a "
        "core is the scheduler's decision where qb and the floor put each half of the grid on its "
        "own core -- see benchmarks/savina/sor.md");

    return qvo::run(argc, argv, std::move(spec), savina_sor_caf::body);
}
