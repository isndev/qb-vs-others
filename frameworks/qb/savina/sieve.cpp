// @benchmark     savina/sieve
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md -- "Dynamic actor creation" (`addRefActor<T>()` from inside an
//                actor creates a child ON THE SAME VirtualCore, its `onInit` run synchronously,
//                the handle's `id()` valid -- and pushable -- before the call returns) and
//                "`push<T>()` is ordered" -- plus the fork-join-create adapter beside this file,
//                whose creation path this is.
// @idiom-note    Both placements of the spec's `place` axis. place=0 is the reference's: a filter
//                that needs a successor creates it with `addRefActor` and pushes to it at once, so
//                every filter lives on the first filter's core. qb has no cross-core spawn, so
//                place=1 asks a per-core Spawner actor (created before start) for filter f on core
//                (f + 1) % cores: the filter sends Spawn, holds the candidates and the Exit it
//                would pass on until the Spawned answer names the new filter, then pushes them in
//                order. Every candidate and Exit is a `push`: a composite that overtook its
//                smallest factor would be kept as a prime.

#include <qvospec/savina/sieve.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_sieve_qb {

using namespace qvospec::savina::sieve;

struct Candidate : qb::Event {
    std::uint64_t number{0};
    explicit Candidate(std::uint64_t n) noexcept : number(n) {}
};
struct Exit : qb::Event {};
struct Ready : qb::Event {};
struct Start : qb::Event {};
struct Spawn : qb::Event {  // place=1: create filter `index` holding `prime`, on this core
    std::uint64_t prime{0};
    std::uint32_t index{0};
    Spawn(std::uint64_t p, std::uint32_t i) noexcept : prime(p), index(i) {}
};
struct Spawned : qb::Event {
    qb::ActorId filter;
    explicit Spawned(qb::ActorId f) noexcept : filter(f) {}
};

// The run's shared, read-only wiring: ids filled before the engine starts, the slots and the
// primes' vectors the body owns (one per filter, each written only by its filter).
struct Field {
    qb::ActorId                              producer;
    qb::ActorId                              first;
    std::vector<qb::ActorId>                 spawners;  // place=1: one per core
    std::vector<Tally>                      *tallies{nullptr};
    std::vector<std::vector<std::uint64_t>> *primes{nullptr};
    std::uint64_t                            local{1};
    long long                                place{0};
    int                                      cores{1};
    qvo::Watch                              *watch{nullptr};
    std::uint64_t                           *starts{nullptr};
};

// A filter's tally is its own state, beside the members every candidate reads: an actor object
// lives in its core's thread arena, so no actor of another core shares its line (sieve::Counts).
// The slot, a line of its own in the body's vector, receives it when the engine tears it down.
class FilterActor final : public qb::Actor {
    const Field               &_field;
    const std::uint32_t        _index;
    Counts                     _counts;
    Filter                     _filter;
    qb::ActorId                _next;
    bool                       _pending{false};  // place=1: a Spawn is out, its answer not in
    bool                       _exit_held{false};
    std::vector<std::uint64_t> _held;  // place=1: what goes to the successor once it exists

    void grow(std::uint64_t prime) {
        const std::uint32_t next = _index + 1;
        if (next >= _field.tallies->size()) too_many_filters();
        if (_field.place == 0) {
            const auto child = addRefActor<FilterActor>(std::cref(_field), next, prime);
            if (!child.valid())
                qvo::fail("savina/sieve qb: addRefActor returned an invalid handle -- the "
                          "VirtualCore's actor id pool is exhausted (see benchmarks/savina/sieve.md)");
            _next = child.id();
            return;
        }
        _pending = true;
        push<Spawn>(_field.spawners[static_cast<std::size_t>(
                        core_of_filter(next, _field.place, _field.cores))],
                    prime, next);
    }

public:
    FilterActor(const Field &field, std::uint32_t index, std::uint64_t prime)
        : _field(field)
        , _index(index)
        , _filter(prime, field.local, _counts, (*field.primes)[index]) {}

    ~FilterActor() noexcept override {
        static_cast<Counts &>((*_field.tallies)[_index]) = _counts;
    }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Candidate>(*this);
        registerEvent<Exit>(*this);
        registerEvent<Spawned>(*this);
        if (_index == 0) push<Ready>(_field.producer);  // the first filter, created before start
        co_return true;
    }

    void on(Candidate const &event) {
        switch (_filter.take(event.number)) {
        case Filter::Verdict::pass:
            if (_pending) _held.push_back(event.number);
            else push<Candidate>(_next, event.number);
            break;
        case Filter::Verdict::grow: grow(event.number); break;
        default: break;  // dropped, or kept here
        }
    }

    void on(Spawned const &event) {
        _next    = event.filter;
        _pending = false;
        for (const std::uint64_t n : _held) push<Candidate>(_next, n);
        _held.clear();
        if (_exit_held) push<Exit>(_next);
    }

    // The Exit follows the last candidate down the chain; the last filter's ends the run.
    void on(Exit const &) {
        ++_counts.exits;
        if (!_filter.linked()) {
            _field.watch->stop();
            broadcast<qb::KillEvent>();
        } else if (_pending) {
            _exit_held = true;
        } else {
            push<Exit>(_next);
        }
    }
};

// place=1: one per core, created before start; creates the filters asked of its core.
class Spawner final : public qb::Actor {
    const Field &_field;

public:
    explicit Spawner(const Field &field) noexcept : _field(field) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Spawn>(*this);
        push<Ready>(_field.producer);
        co_return true;
    }

    void on(Spawn const &event) {
        const auto child = addRefActor<FilterActor>(std::cref(_field), event.index, event.prime);
        if (!child.valid())
            qvo::fail("savina/sieve qb: addRefActor returned an invalid handle -- the VirtualCore's "
                      "actor id pool is exhausted (see benchmarks/savina/sieve.md)");
        push<Spawned>(event.getSource(), child.id());
    }
};

class Producer final : public qb::Actor {
    const Field        &_field;
    const std::uint64_t _limit;
    std::size_t         _ready{0};
    std::size_t         _expect;

public:
    Producer(const Field &field, std::uint64_t limit, std::size_t expect) noexcept
        : _field(field), _limit(limit), _expect(expect) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Start>(*this);
        co_return true;
    }

    // The first filter (and, with place=1, every spawner) is up: every core running.
    void on(Ready const &) {
        if (++_ready != _expect) return;
        _field.watch->start();
        push<Start>(id());
    }

    // Savina's NumberProducerActor: every candidate from this one handler, then the Exit.
    void on(Start const &) {
        ++*_field.starts;
        for (std::uint64_t n = 3; n < _limit; n += 2) push<Candidate>(_field.first, n);
        push<Exit>(_field.first);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto limit  = static_cast<std::uint64_t>(p.get("limit"));
    const auto place  = p.get("place");
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;
    const auto nfilt  = static_cast<std::size_t>(filters(p));
    const int  ncores = cores < 1 ? 1 : cores;

    std::vector<Tally>                      tallies(nfilt);
    std::vector<std::vector<std::uint64_t>> primes(nfilt);
    std::uint64_t                           starts = 0;
    Field                                   field;
    field.tallies = &tallies;
    field.primes  = &primes;
    field.local   = static_cast<std::uint64_t>(p.get("local"));
    field.place   = place;
    field.cores   = ncores;
    field.watch   = &watch;
    field.starts  = &starts;
    {
        qb::Main engine;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        std::size_t ready = 1;  // the first filter
        if (place == 1) {
            for (int c = 0; c < ncores; ++c)
                field.spawners.push_back(engine.addActor<Spawner>(static_cast<qb::CoreId>(c),
                                                                  std::cref(field)));
            ready += field.spawners.size();
        }
        field.producer = engine.addActor<Producer>(0, std::cref(field), limit, ready);
        field.first    = engine.addActor<FilterActor>(
            static_cast<qb::CoreId>(core_of_filter(0, place, ncores)), std::cref(field),
            std::uint32_t{0}, std::uint64_t{2});

        engine.start();
        engine.join();
    }
    // The engine is gone: every filter handed its counts to its slot as it was destroyed, and the
    // tallies and the primes are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, primes), delivered(tallies, starts)};
}

}  // namespace savina_sieve_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::sieve::params();
    // The placement this adapter's table cell runs: the reference's (place=0) until the quiet-host
    // measurement of both forms names the faster one (benchmarks/savina/sieve.md).
    spec.params["place"]   = 0;
    spec.expected          = qvospec::savina::sieve::expected;
    spec.expected_messages = qvospec::savina::sieve::expected_messages;
    spec.work_unit         = qvospec::savina::sieve::kWorkUnit;
    spec.work_units        = qvospec::savina::sieve::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md addRefActor<T>() (same-core child, onInit run "
                             "synchronously) + push<T>() (ordered); the fork-join-create adapter";
    spec.idiom_note        = "place=0: a filter creates its successor with addRefActor, every "
                             "filter on the first filter's core; place=1: filter f created on core "
                             "(f + 1) % cores by that core's Spawner, the candidates held until its "
                             "id arrives; every candidate pushed (ordered)";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb has no cross-core spawn: with place=0 -- the reference's, a filter creating its own "
        "successor -- every filter lives on the first filter's core (core 1 at cores=2, the "
        "producer on core 0); place=1 spreads them through a Spawner per core and an extra round "
        "trip per creation. The run's `place` is in its params -- see benchmarks/savina/sieve.md");
    spec.caveats.emplace_back(
        "qb's actor id is a 24-bit ordinary slot per VirtualCore since 3.3.0 (16 711 679 live actors; "
        "3.2.1's was a 16-bit slot, 65 534): with place=0 every filter is on one core, so a run "
        "needing more filters than that fails the cell");

    return qvo::run(argc, argv, std::move(spec), savina_sieve_qb::body);
}
