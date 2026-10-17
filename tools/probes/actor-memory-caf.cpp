// CAF's existing stateful behavior and scheduled worker pool; no detached actors.
#include <qvo/actor-memory.h>
#include "../../frameworks/caf/caf_support.h"
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/anon_mail.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

namespace actor_memory_caf {
using qvo::probe::PopulationLedger;
struct ResidentState {
    PopulationLedger *ledger = nullptr;
    std::size_t index = 0;
    ~ResidentState() { if (ledger) ledger->destroyed(index); }
};

caf::behavior resident(caf::stateful_actor<ResidentState> *self, PopulationLedger *ledger, std::size_t index) {
    self->state().ledger = ledger;
    self->state().index = index;
    ledger->born(index);
    self->mail(caf::tick_atom_v).send(self);
    return {
        [self](caf::tick_atom) { self->state().ledger->ready(self->state().index); },
        [self](caf::close_atom) { self->quit(); },
    };
}
caf::behavior sentinel(caf::event_based_actor *self, PopulationLedger *ledger, unsigned index) {
    self->mail(caf::tick_atom_v).send(self);
    return {
        [ledger, index](caf::tick_atom) { ledger->infrastructure_ready_signal(index); },
        [self](caf::close_atom) { self->quit(); },
    };
}
} // namespace actor_memory_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();
    std::vector<caf::actor> handles;
    qvo::probe::MemoryAdapter adapter;
    adapter.framework = "caf";
    adapter.version = QVO_FRAMEWORK_VERSION;
    adapter.caveat = "Scheduled CAF pool, fixed max-threads; one infrastructure actor per worker budget. "
                     "Observer handles are allocated before baseline and released after shutdown; auxiliary framework threads remain. "
                     "sizeof is the stateful actor type, not its allocator/control-block/behavior extent.";
    adapter.floor = false;
    adapter.object_size = sizeof(caf::stateful_actor<actor_memory_caf::ResidentState>);
    adapter.prepare = [&](const auto &config) { handles.resize(config.actors + config.cores); };
    adapter.run = [&](const auto &config, auto &ledger, const auto &capture) {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, config.cores, false);
        caf::actor_system system{cfg};
        qvocaf::assert_budget(system, config.cores);
        auto stop = [&] {
            for (const auto &actor : handles) if (actor) caf::anon_mail(caf::close_atom_v).send(actor);
            system.await_all_actors_done();
            for (auto &actor : handles) actor = nullptr;
        };
        try {
            for (unsigned c = 0; c < config.cores; ++c)
                handles[c] = system.spawn(actor_memory_caf::sentinel, &ledger, c);
            for (std::size_t i = 0; i < qvo::probe::memory_spawn_count<(QVO_MEMORY_CONTROL_DROP_LAST != 0)>(config.actors); ++i)
                handles[config.cores + i] = system.spawn(actor_memory_caf::resident, &ledger, i);
            capture();
        } catch (...) { stop(); throw; }
        stop();
        qvocaf::assert_pins_took();
    };
    return qvo::probe::actor_memory_main(argc, argv, adapter);
}
