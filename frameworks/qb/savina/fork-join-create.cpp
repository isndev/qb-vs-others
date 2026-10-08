// @benchmark     savina/fork-join-create
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("Dynamic actor creation": `addRefActor<T>()` from inside an
//                actor creates a child ON THE SAME VirtualCore, its `onInit` run synchronously,
//                and the handle's `id()` is valid -- and pushable -- before the call returns) and
//                `kill()`; the fib adapter beside this file, whose child lifecycle this is.
// @idiom-note    A creator forks its share in one loop: `addRefActor<ForkActor>(id(), i, work)`
//                then `push<Job>` to the new id, the handle discarded. A forked actor answers its
//                creator with one `push<Done>` and `kill()`s itself in the same handler. qb has
//                NO cross-core spawn primitive, so the spec's one-creator-per-core structure is
//                what spreads the actors: creator s on VirtualCore s % cores, its actors on its
//                core. An invalid child id is the per-core 16-bit slot pool exhausted -- a hard
//                failure, never folded into a checksum that could happen to match.

#include <qvospec/savina/fork-join-create.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace savina_fork_join_create_qb {

using namespace qvospec::savina::fork_join_create;

struct Ready : qb::Event {};
struct Fork : qb::Event {};
struct Job : qb::Event {
    std::uint64_t index{0};
    explicit Job(std::uint64_t i) noexcept : index(i) {}
};
struct Done : qb::Event {
    std::uint64_t value{0};
    std::uint64_t messages{0};
    Done(std::uint64_t v, std::uint64_t m) noexcept : value(v), messages(m) {}
};
struct Summary : qb::Event {
    std::uint64_t chk{0};
    std::uint64_t messages{0};
    Summary(std::uint64_t c, std::uint64_t m) noexcept : chk(c), messages(m) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

// The creators' ids, filled before the engine starts (ids are assigned at addActor time, the
// actors themselves are constructed at start) and read-only from then on.
struct Field {
    std::vector<qb::ActorId> creators;
};

// One forked actor: created by its creator, sent one job, answers, terminates.
class ForkActor final : public qb::Actor {
    const qb::ActorId   _creator;
    const std::uint64_t _self;
    const int           _work;

public:
    ForkActor(qb::ActorId creator, std::uint64_t self, int work) noexcept
        : _creator(creator), _self(self), _work(work) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Job>(*this);
        co_return true;
    }

    void on(Job const &event) {
        push<Done>(_creator, job_value(_self, event.index, _work), 1);
        kill();
    }
};

// One creator per core: forks the indices i with i % creators == s, folds their answers, reports
// one summary to the driver.
class CreatorActor final : public qb::Actor {
    const qb::ActorId   _driver;
    const std::uint64_t _first;
    const std::uint64_t _stride;
    const std::uint64_t _actors;
    const std::uint64_t _share;
    const int           _work;
    std::uint64_t       _acc{0};
    std::uint64_t       _messages{0};
    std::uint64_t       _done{0};

public:
    CreatorActor(qb::ActorId driver, std::uint64_t first, std::uint64_t stride,
                 std::uint64_t actors, int work) noexcept
        : _driver(driver)
        , _first(first)
        , _stride(stride)
        , _actors(actors)
        , _share(share(actors, stride, first))
        , _work(work) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Fork>(*this);
        registerEvent<Done>(*this);
        push<Ready>(_driver);
        co_return true;
    }

    void on(Fork const &) {
        ++_messages;
        for (std::uint64_t i = _first; i < _actors; i += _stride) {
            const auto child = addRefActor<ForkActor>(id(), i, _work);
            if (!child.valid()) {
                // The per-VirtualCore ServiceIdPool is 16-bit: 65 534 live actors. A creator's
                // whole share is alive at once (its actors run only once this handler returns),
                // so exhausting it is a qb limit this benchmark exposes, and a limit is a FAILED
                // cell, not a checksum that happens to still add up.
                std::fprintf(stderr,
                             "savina/fork-join-create qb: addRefActor returned an invalid handle "
                             "-- the VirtualCore's 16-bit actor id pool is exhausted (actors too "
                             "large for one core; see benchmarks/savina/fork-join-create.md)\n");
                std::abort();
            }
            push<Job>(child.id(), i);
        }
        if (_share == 0) push<Summary>(_driver, _acc, _messages);  // more creators than actors
    }

    void on(Done const &event) {
        _acc += event.value;
        _messages += 1 + event.messages;
        if (++_done == _share) push<Summary>(_driver, _acc, _messages);
    }
};

// The driver: opens the window once every creator is up, closes it on the last summary.
class DriverActor final : public qb::Actor {
    const Field &_field;
    qvo::Watch  &_watch;
    Sink        &_sink;
    std::size_t  _ready{0};
    std::size_t  _done{0};

public:
    DriverActor(const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Summary>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.creators.size()) return;
        _watch.start();
        for (const auto &c : _field.creators) push<Fork>(c);
    }

    void on(Summary const &event) {
        _sink.checksum += event.chk;
        _sink.messages += 1 + event.messages;
        if (++_done == _field.creators.size()) {
            _watch.stop();
            broadcast<qb::KillEvent>();
        }
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors = static_cast<std::uint64_t>(p.get("actors"));
    const auto work   = static_cast<int>(p.get("work"));
    const auto ncreat = creators(p);
    const bool spin   = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = static_cast<int>(ncreat);
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        const auto driver = engine.addActor<DriverActor>(0, std::cref(field), std::ref(watch),
                                                         std::ref(sink));
        field.creators.reserve(ncreat);
        for (std::uint64_t s = 0; s < ncreat; ++s)
            field.creators.push_back(engine.addActor<CreatorActor>(
                static_cast<qb::CoreId>(s), driver, s, ncreat, actors, work));

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fork_join_create_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join_create::params();
    spec.expected          = qvospec::savina::fork_join_create::expected;
    spec.expected_messages = qvospec::savina::fork_join_create::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join_create::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join_create::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: addRefActor<T>() (same-core child, onInit run "
                             "synchronously) + kill(); the fib adapter's child lifecycle";
    spec.idiom_note        = "creator s on VirtualCore s forks its share with addRefActor + "
                             "push<Job> in one loop; a forked actor answers its creator with one "
                             "push<Done> and kills itself";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb has no cross-core spawn: a forked actor lives on its creator's VirtualCore, so with "
        "cores=2 each core forks and runs exactly its own half and nothing balances the two");
    spec.caveats.emplace_back(
        "a creator's whole share is alive at once -- its actors run only once the forking loop "
        "returns -- and qb's actor id is a 16-bit slot per VirtualCore (65 534 live actors): "
        "actors=40 000 fits on one core, a share above ~65 500 would abort the cell");

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_create_qb::body);
}
