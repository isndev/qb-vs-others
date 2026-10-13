// @benchmark     savina/radixsort
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md -- "`push<T>()` is ordered" (FIFO per sender and receiver: here
//                the order IS the sort) -- and the thread-ring adapter beside this file (ids known
//                before start(), a tally kept in the actor's own state and handed to its slot at
//                teardown).
// @idiom-note    The reference's actors one for one: a source that draws every value in ONE
//                handler, `bits` sort stages, a validation. A value whose bit is 0 goes on with
//                `push<Value>` at once, a held one is pushed in the flush of the stage's last
//                arrival. `push`, never `forward`/`send`: those are eager and unordered across
//                cores, and a stage's output must keep its arrival order. The actors in pipeline
//                order are cut into `cores` contiguous blocks (radixsort::core_of), so with
//                cores=2 a value crosses a core once, between the two halves of the chain.

#include <qvospec/savina/radixsort.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_radixsort_qb {

using namespace qvospec::savina::radixsort;

struct Value : qb::Event {
    std::uint64_t value{0};
    explicit Value(std::uint64_t v) noexcept : value(v) {}
};
struct Ready : qb::Event {};
struct Start : qb::Event {};

struct Sink {
    std::uint64_t starts{0};     // the Starts the source's handler ran
    std::uint64_t validated{0};  // the values the validation took
};

// The ids, filled before the engine starts (ids are assigned at addActor time, the actors are
// constructed at start) and read-only from then on.
struct Chain {
    qb::ActorId              source;
    std::vector<qb::ActorId> stages;
    qb::ActorId              validation;
};

class SourceActor final : public qb::Actor {
    const Chain        &_chain;
    const std::uint64_t _values;
    const long long     _seed;
    const long long     _bits;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::size_t         _ready{0};

public:
    SourceActor(const Chain &chain, std::uint64_t values, long long seed, long long bits,
                qvo::Watch &watch, Sink &sink) noexcept
        : _chain(chain), _values(values), _seed(seed), _bits(bits), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Start>(*this);
        co_return true;
    }

    // The window opens once every stage and the validation have reported in: every core running.
    void on(Ready const &) {
        if (++_ready != _chain.stages.size() + 1) return;
        _watch.start();
        push<Start>(id());
    }

    // Savina's IntSourceActor: every value from this one handler.
    void on(Start const &) {
        ++_sink.starts;
        Source            source(_seed, _bits);
        const qb::ActorId first = _chain.stages.front();
        for (std::uint64_t i = 0; i < _values; ++i) push<Value>(first, source.next());
    }
};

// The tally is the actor's own state, beside the members every value reads: an actor object lives
// in its core's thread arena, so no actor of another core shares its line (radixsort::Counts). The
// slot, a line of its own in the body's vector, receives it when the engine tears the actor down.
class StageActor final : public qb::Actor {
    const Chain               &_chain;
    const std::uint32_t        _index;
    Counts                     _counts;
    Stage                      _stage;
    qb::ActorId                _next;
    std::vector<std::uint64_t> _held;
    Tally                     &_slot;

public:
    StageActor(const Chain &chain, std::uint32_t index, std::uint64_t values, Tally &slot)
        : _chain(chain), _index(index), _stage(index, values, _counts), _slot(slot) {
        _held.reserve(static_cast<std::size_t>(values));
    }

    ~StageActor() noexcept override { static_cast<Counts &>(_slot) = _counts; }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        _next = _index + 1 < _chain.stages.size() ? _chain.stages[_index + 1] : _chain.validation;
        push<Ready>(_chain.source);
        co_return true;
    }

    void on(Value const &event) {
        const std::uint64_t v = event.value;
        if (_stage.take(v)) push<Value>(_next, v);
        else _held.push_back(v);
        if (_stage.complete())
            for (const std::uint64_t h : _held) push<Value>(_next, h);
    }
};

class ValidationActor final : public qb::Actor {
    const Chain &_chain;
    Validation   _validation;
    qvo::Watch  &_watch;
    Sink        &_sink;

public:
    ValidationActor(const Chain &chain, std::uint64_t *sequence, std::uint64_t values,
                    qvo::Watch &watch, Sink &sink) noexcept
        : _chain(chain), _validation(sequence, values), _watch(watch), _sink(sink) {}

    ~ValidationActor() noexcept override { _sink.validated = _validation.count(); }

    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        push<Ready>(_chain.source);
        co_return true;
    }

    // The last value ends the run; one after it fails the run (radixsort::overflow).
    void on(Value const &event) {
        if (!_validation.take(event.value)) return;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto values = static_cast<std::uint64_t>(p.get("values"));
    const auto bits   = static_cast<std::uint32_t>(p.get("bits"));
    const auto cores  = static_cast<int>(p.get("cores"));
    const bool spin   = p.get("wait") != 0;

    Sink                       sink;
    Chain                      chain;
    std::vector<Tally>         tallies(bits);
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(values));
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);
        const std::uint32_t npos  = positions(bits);
        const auto          place = [&](std::uint32_t position) {
            return static_cast<qb::CoreId>(core_of(position, npos, ncores));
        };

        chain.source = engine.addActor<SourceActor>(place(0), std::cref(chain), values,
                                                    p.get("seed"), p.get("bits"), std::ref(watch),
                                                    std::ref(sink));
        chain.stages.reserve(bits);
        for (std::uint32_t k = 0; k < bits; ++k)
            chain.stages.push_back(engine.addActor<StageActor>(place(k + 1), std::cref(chain), k,
                                                               values, std::ref(tallies[k])));
        chain.validation = engine.addActor<ValidationActor>(place(bits + 1), std::cref(chain),
                                                            sequence.data(), values,
                                                            std::ref(watch), std::ref(sink));

        engine.start();
        engine.join();
    }
    // The engine is gone: every stage handed its counts to its slot as it was destroyed, and the
    // tallies and the sequence are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence), delivered(tallies, sink.validated, sink.starts)};
}

}  // namespace savina_radixsort_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::radixsort::params();
    spec.expected          = qvospec::savina::radixsort::expected;
    spec.expected_messages = qvospec::savina::radixsort::expected_messages;
    spec.work_unit         = qvospec::savina::radixsort::kWorkUnit;
    spec.work_units        = qvospec::savina::radixsort::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md push<T>() (ordered) + the thread-ring adapter";
    spec.idiom_note        = "the reference's actors one for one; every value pushed (ordered) from "
                             "stage to stage, the held ones in the flush of a stage's last arrival; "
                             "the chain cut into `cores` contiguous blocks";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "the source, the stages and the validation, in pipeline order, are cut into `cores` "
        "contiguous blocks (radixsort::core_of): with cores=2 the first half of the chain runs on "
        "core 0 and the second on core 1, and a value crosses a core once. CAF and SObjectizer "
        "receive the same budget and their pools place every actor -- see "
        "benchmarks/savina/radixsort.md");
    spec.caveats.emplace_back(
        "the source draws every value in ONE handler, as the reference's IntSourceActor does, and a "
        "stage pushes its held values in the handler of its last arrival: qb publishes a handler's "
        "pushes after it returns (the pass's flush), so a stage's output leaves in bursts");

    return qvo::run(argc, argv, std::move(spec), savina_radixsort_qb::body);
}
