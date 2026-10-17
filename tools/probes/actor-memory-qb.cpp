// Idle live population. Same actor creation/dispatch idiom as qb Savina adapters.
#include <qvo/actor-memory.h>
#include "../../frameworks/qb/qb_support.h"
#include <qb/main.h>
#include <qb/system/allocator/region.h>
#include <qb/system/allocator/thread_arena.h>
#include <stdexcept>

namespace actor_memory_qb {
using qvo::probe::PopulationLedger;
struct Check : qb::Event {};

// Allocated/touched by prepare before the first memory reading; no resize while the cores run.
// Each Sentinel writes only its own slot. The controller reads after join.
std::vector<qvo::probe::CoreArenaMemory> arena_observations;

class Resident final : public qb::Actor {
    PopulationLedger &_ledger;
    std::size_t _index;
public:
    Resident(PopulationLedger &ledger, std::size_t index) : _ledger(ledger), _index(index) { _ledger.born(_index); }
    ~Resident() final { _ledger.destroyed(_index); }
    qb::io::async::task<bool> onInit() final {
        registerEvent<Check>(*this);
        push<Check>(id());
        co_return true;
    }
    void on(Check const &) { _ledger.ready(_index); }
};

// Fixed infrastructure, present even at N=0. Not included in the target population.
class Sentinel final : public qb::Actor {
    PopulationLedger &_ledger;
    unsigned _core;
public:
    Sentinel(PopulationLedger &ledger, unsigned core) : _ledger(ledger), _core(core) {}
    qb::io::async::task<bool> onInit() final {
        registerEvent<Check>(*this);
        push<Check>(id());
        co_return true;
    }
    void on(Check const &) {
        // Main::start_thread constructs all local factories and finishes their
        // synchronous init before entering workflow. This Check is dispatched
        // only there, so the local target population has already been built.
        using qb::allocator::thread_arena;
        arena_observations[_core] = {_core, thread_arena::live(), thread_arena::chunks(),
                                     thread_arena::chunk_bytes(), true};
        _ledger.infrastructure_ready_signal(_core);
    }
};

// The one infrastructure object is in the measured pooled arena. This establishes
// a nonempty snapshot; it does not establish live_blocks == local targets + 1.
static_assert(sizeof(Sentinel) <= qb::allocator::thread_arena::max_small);
static_assert(alignof(Sentinel) <= qb::allocator::thread_arena::granule);

void run(const qvo::probe::MemoryConfig &config, PopulationLedger &ledger, const std::function<void()> &capture) {
    qb::Main engine;
    for (unsigned c = 0; c < config.cores; ++c) {
        qvoqb::configure_core(engine, static_cast<int>(c), false);
        engine.addActor<Sentinel>(static_cast<qb::CoreId>(c), std::ref(ledger), c);
    }
    for (std::size_t i = 0; i < qvo::probe::memory_spawn_count<(QVO_MEMORY_CONTROL_DROP_LAST != 0)>(config.actors); ++i)
        engine.addActor<Resident>(static_cast<qb::CoreId>(i % config.cores), std::ref(ledger), i);
    engine.start(true);
    try { capture(); }
    catch (...) { engine.stop(); engine.join(); throw; }
    engine.stop();
    engine.join();
    if (engine.hasError()) throw std::runtime_error("qb engine reported an error after join");
}
} // namespace actor_memory_qb

int main(int argc, char **argv) {
    qvo::probe::MemoryAdapter adapter;
    adapter.framework = "qb";
    adapter.version = QVO_FRAMEWORK_VERSION;
    adapter.caveat = "Fixed core count plus one infrastructure actor per core; default actor subscriptions and one Check subscription; "
                     "sizeof includes the ledger reference and index. Region counters are process-global, not actor attribution. "
                     "Whole owner-thread arena counters are sampled at Sentinel readiness, after local startup init, not simultaneously with RSS. "
                     "Custom subscriptions use retained dense tables without a per-population byte accessor; onInit frame bytes are unavailable.";
    adapter.floor = false;
    adapter.object_size = sizeof(actor_memory_qb::Resident);
    adapter.prepare = [](const auto &config) { actor_memory_qb::arena_observations.resize(config.cores); };
    adapter.qb_thread_arenas = &actor_memory_qb::arena_observations;
    adapter.regions = []() -> std::optional<qvo::probe::RegionMemory> {
        const auto r = qb::allocator::detail::region_stats();
        return qvo::probe::RegionMemory{r.cached_bytes, r.lent_bytes, r.peak_bytes};
    };
    adapter.run = actor_memory_qb::run;
    return qvo::probe::actor_memory_main(argc, argv, adapter);
}
