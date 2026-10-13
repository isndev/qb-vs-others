// @benchmark     savina/recmatmul
// @framework     qb
// @idiom-source  the nqueens adapter beside this file (a master that opens the window once every
//                worker reported ready, the done-count termination push<>'s per-sender order makes
//                sound, broadcast<qb::KillEvent>() to end the run) and the a-star adapter's relay:
//                Actor.h forward() ("delegate an event to another actor without creating a new
//                event").
// @idiom-note    The master and the `workers` workers are created before start(); worker w lives on
//                VirtualCore floor(w x cores / workers) -- contiguous blocks, the spec says why --
//                the master on core 0. A worker answers a Task event by splitting it (eight Task
//                events push<>ed to the master) or multiplying it (the shared kernel, into its
//                quarter of C), then push<>es one Done -- all on one ordered channel, which is what
//                makes "done == sent" a sound end. The master relays each created task to the
//                worker Savina's routing names with forward(): the same event, no copy.

#include <qvospec/savina/recmatmul.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_recmatmul_qb {

using namespace qvospec::savina::recmatmul;

struct Work : qb::Event {
    Task task;
    explicit Work(const Task &t) noexcept : task(t) {}
};
struct Done : qb::Event {};
struct Ready : qb::Event {};

// Filled before the engine starts (addActor assigns ids before the actors are constructed) and
// read-only from then on.
struct Field {
    qb::ActorId              master;
    std::vector<qb::ActorId> workers;
};

// The worker's counts are the actor's own state, on the line its handler already touches
// (recmatmul::Tally says why); its slot receives them when the engine tears the actor down.
class WorkerActor final : public qb::Actor {
    const Field   &_field;
    const Config   _config;
    Matrices      &_matrices;
    Counts         _counts;
    Tally         &_slot;

public:
    WorkerActor(const Field &field, const Config &config, Matrices &matrices, Tally &slot) noexcept
        : _field(field), _config(config), _matrices(matrices), _slot(slot) {}

    ~WorkerActor() noexcept override { static_cast<Counts &>(_slot) = _counts; }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Work>(*this);
        push<Ready>(_field.master);
        co_return true;
    }

    void on(Work const &event) {
        ++_counts.works;
        if (is_leaf(event.task, _config)) {
            ++_counts.leaves;
            multiply(_matrices, event.task);
        } else {
            ++_counts.splits;
            const qb::ActorId master = _field.master;
            split(event.task, [&](const Task &child) { push<Work>(master, child); });
        }
        push<Done>(_field.master);
    }
};

class MasterActor final : public qb::Actor {
    const Field   &_field;
    const Config   _config;
    qvo::Watch    &_watch;
    MasterCounts  &_counts;
    std::size_t    _ready{0};

public:
    MasterActor(const Field &field, const Config &config, qvo::Watch &watch,
                MasterCounts &counts) noexcept
        : _field(field), _config(config), _watch(watch), _counts(counts) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Work>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    // The window opens once every worker has reported in: every core running and warm.
    void on(Ready const &) {
        if (++_ready != _field.workers.size()) return;
        _watch.start();
        const Task whole = root(_config);
        ++_counts.sent;
        push<Work>(_field.workers[route(whole, _config)], whole);
    }

    // A task a worker created: relayed, as received, to the worker Savina's routing names.
    void on(Work &event) {
        ++_counts.created;
        ++_counts.sent;
        forward(_field.workers[route(event.task, _config)], event);
    }

    void on(Done const &) {
        if (++_counts.dones != _counts.sent) return;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<int>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;

    Matrices           matrices(c.length);  // A, B and a zero C, built before the engine
    MasterCounts       counts;
    std::vector<Tally> tallies(c.workers);
    Field              field;
    {
        qb::Main engine;

        const unsigned ncores = cores < 1 ? 1u : static_cast<unsigned>(cores);
        for (unsigned k = 0; k < ncores; ++k) qvoqb::configure_core(engine, static_cast<int>(k), spin);

        field.master = engine.addActor<MasterActor>(0, std::cref(field), c, std::ref(watch),
                                                    std::ref(counts));
        field.workers.reserve(c.workers);
        for (std::uint32_t w = 0; w < c.workers; ++w)
            field.workers.push_back(engine.addActor<WorkerActor>(
                static_cast<qb::CoreId>(core_of(w, c.workers, ncores)), std::cref(field), c,
                std::ref(matrices), std::ref(tallies[w])));

        engine.start();
        engine.join();
    }
    // The engine is gone: every worker handed its counts to its slot as it was destroyed, and C,
    // the tallies and the master's counts are reduced here, outside the window.
    return qvo::Answer{checksum(matrices, tallies, counts), delivered(tallies, counts)};
}

}  // namespace savina_recmatmul_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::recmatmul::params();
    spec.expected          = qvospec::savina::recmatmul::expected;
    spec.expected_messages = qvospec::savina::recmatmul::expected_messages;
    spec.work_unit         = qvospec::savina::recmatmul::kWorkUnit;
    spec.work_units        = qvospec::savina::recmatmul::work_units;
    spec.idiom_source      = "the nqueens adapter (ready handshake, push<> ordered per sender, "
                             "done-count end) + Actor.h forward() for the master's relay (a-star)";
    spec.idiom_note        = "worker w on VirtualCore floor(w x cores / workers) splits (eight Work "
                             "push<>ed to the master) or multiplies (the shared kernel), then one "
                             "Done; the master on core 0 forward()s each created task to worker "
                             "(srC + scC) % workers";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "worker w lives on VirtualCore floor(w x cores / workers), fixed before start, and Savina's "
        "routing fixes which worker each task goes to: at the defaults every leaf goes to workers "
        "0, 4, 8, 12 and 16, so with cores=2 core 0 multiplies 304 of the 512 leaves and core 1 "
        "208 -- the best split of those five workers over two cores, and never rebalanced; the "
        "pools of CAF and SObjectizer place each worker's demands dynamically");

    return qvo::run(argc, argv, std::move(spec), savina_recmatmul_qb::body);
}
