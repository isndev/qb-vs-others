// Same individual-FIFO pool binder as the many-agent Savina adapters.
#include <qvo/actor-memory.h>
#include "../../frameworks/sobjectizer/so_support.h"
#include <so_5/wrapped_env.hpp>

namespace actor_memory_so {
using qvo::probe::PopulationLedger;
struct Check final : so_5::signal_t {};
class Resident final : public so_5::agent_t {
    PopulationLedger &_ledger;
    std::size_t _index;
public:
    Resident(context_t context, PopulationLedger &ledger, std::size_t index)
        : so_5::agent_t(context), _ledger(ledger), _index(index) { _ledger.born(_index); }
    ~Resident() final { _ledger.destroyed(_index); }
    void so_define_agent() final {
        so_subscribe_self().event([this](so_5::mhood_t<Check>) { _ledger.ready(_index); });
    }
    void so_evt_start() final { so_5::send<Check>(*this); }
};
class Sentinel final : public so_5::agent_t {
    PopulationLedger &_ledger;
    unsigned _index;
public:
    Sentinel(context_t context, PopulationLedger &ledger, unsigned index)
        : so_5::agent_t(context), _ledger(ledger), _index(index) {}
    void so_define_agent() final {
        so_subscribe_self().event([this](so_5::mhood_t<Check>) { _ledger.infrastructure_ready_signal(_index); });
    }
    void so_evt_start() final { so_5::send<Check>(*this); }
};
} // namespace actor_memory_so

int main(int argc, char **argv) {
    qvo::probe::MemoryAdapter adapter;
    adapter.framework = "sobjectizer";
    adapter.version = QVO_FRAMEWORK_VERSION;
    adapter.caveat = "All agents share one cooperation because their lifetimes end together; individual FIFO on a fixed worker pool "
                     "(one_thread for one worker), one infrastructure agent per worker budget. Auxiliary environment threads remain.";
    adapter.floor = false;
    adapter.object_size = sizeof(actor_memory_so::Resident);
    adapter.run = [](const auto &config, auto &ledger, const auto &capture) {
        so_5::wrapped_env_t environment{so_5::wrapped_env_t::wait_init_completion,
            [&](so_5::environment_t &env) {
                auto binder = qvoso::make_pool_binder(env, static_cast<int>(config.cores), false);
                env.introduce_coop(binder, [&](so_5::coop_t &coop) {
                    for (unsigned c = 0; c < config.cores; ++c)
                        coop.make_agent<actor_memory_so::Sentinel>(std::ref(ledger), c);
                    for (std::size_t i = 0; i < qvo::probe::memory_spawn_count<(QVO_MEMORY_CONTROL_DROP_LAST != 0)>(config.actors); ++i)
                        coop.make_agent<actor_memory_so::Resident>(std::ref(ledger), i);
                });
            }};
        capture(); // wrapped_env stops/joins/destroys every agent on either exit path.
    };
    return qvo::probe::actor_memory_main(argc, argv, adapter);
}
