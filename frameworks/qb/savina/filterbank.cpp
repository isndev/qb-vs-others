// @benchmark     savina/filterbank
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md -- "`push<T>()` is ordered" (an Exit must follow every value an
//                actor sent) -- and the thread-ring adapter beside this file (ids known before
//                start(), a tally kept in the actor's own state and handed to its slot at
//                teardown).
// @idiom-note    The reference's actors one for one -- producer, source, branches, seven actors per
//                bank, integrator, combine, sink -- each filter's state the spec's (Delay, Fir,
//                Sampler, Join), every value a `push<Value>` and the Exit pushed behind it. A round
//                travels to the combine as one event carrying its `channels` values (a
//                std::vector, the reference's collection). The producer, the source and the branches
//                live on core 0, bank j on core j % cores, the integrator, the combine and the sink
//                on the last core (filterbank::core_of).

#include <qvospec/savina/filterbank.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_filterbank_qb {

using namespace qvospec::savina::filterbank;

struct Value : qb::Event {
    double value{0};
    explicit Value(double v) noexcept : value(v) {}
};
struct Tagged : qb::Event {
    std::uint32_t channel{0};
    double        value{0};
    Tagged(std::uint32_t c, double v) noexcept : channel(c), value(v) {}
};
struct Round : qb::Event {
    std::vector<double> values;
    explicit Round(std::vector<double> v) noexcept : values(std::move(v)) {}
};
struct Ready : qb::Event {};
struct Next : qb::Event {};
struct Boot : qb::Event {};
struct Exit : qb::Event {};

// The ids, filled before the engine starts (ids are assigned at addActor time, the actors are
// constructed at start) and read-only from then on; actor a is ids[a] (filterbank::bank_actor).
struct Field {
    std::vector<qb::ActorId> ids;
    std::uint32_t            channels{0};
};

// Every actor's tally is its own state, beside the members its handler reads: an actor object
// lives in its core's thread arena, so no actor of another core shares its line. The slot, a line
// of its own in the body's vector, receives it when the engine tears the actor down.
class Tallied : public qb::Actor {
protected:
    const Field  &_field;
    std::uint32_t _self;
    Counts        _counts;
    Tally        &_slot;

    qb::ActorId peer(std::uint32_t actor) const noexcept { return _field.ids[actor]; }

public:
    Tallied(const Field &field, std::uint32_t self, Tally &slot) noexcept
        : _field(field), _self(self), _slot(slot) {}
    ~Tallied() noexcept override { static_cast<Counts &>(_slot) = _counts; }
};

class Producer final : public Tallied {
    const std::uint64_t _simulations;
    qvo::Watch         &_watch;
    std::uint64_t       _sent{0};
    std::size_t         _ready{0};

public:
    Producer(const Field &field, Tally &slot, std::uint64_t simulations, qvo::Watch &watch) noexcept
        : Tallied(field, kProducer, slot), _simulations(simulations), _watch(watch) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Next>(*this);
        co_return true;
    }

    // Every other actor is up: open the window with the reference's first Next.
    void on(Ready const &) {
        if (++_ready != _field.ids.size() - 1) return;
        _watch.start();
        push<Next>(id());
    }

    void on(Next const &) {
        ++_counts.count;
        if (_sent == _simulations) {
            push<Exit>(peer(kSource));
            return;
        }
        ++_sent;
        push<Boot>(peer(kSource));
    }
};

class SourceActor final : public Tallied {
    std::uint64_t _current{0};

public:
    SourceActor(const Field &field, Tally &slot) noexcept : Tallied(field, kSource, slot) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Boot>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Boot const &) {
        ++_counts.count;
        push<Value>(peer(kBranches), sample(_current++));
        push<Next>(peer(kProducer));
    }
    void on(Exit const &) {
        ++_counts.exits;
        push<Exit>(peer(kBranches));
    }
};

class Branches final : public Tallied {
public:
    Branches(const Field &field, Tally &slot) noexcept : Tallied(field, kBranches, slot) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Value const &event) {
        _counts.add(event.value);
        for (std::uint32_t j = 0; j < _field.channels; ++j)
            push<Value>(peer(bank_actor(j, kBank)), event.value);
    }
    void on(Exit const &) {
        ++_counts.exits;
        for (std::uint32_t j = 0; j < _field.channels; ++j) push<Exit>(peer(bank_actor(j, kBank)));
    }
};

// One actor of a bank: `Filter` turns the value it receives into what it sends on (nothing, from an
// FIR not yet full). Its successor is the next actor of the bank.
template <typename Filter>
class BankStage final : public Tallied {
    Filter      _filter;
    qb::ActorId _next;

public:
    BankStage(const Field &field, std::uint32_t self, Tally &slot, Filter filter)
        : Tallied(field, self, slot), _filter(std::move(filter)) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        _next = peer(_self + 1);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Value const &event) {
        _counts.add(event.value);
        double out = 0;
        if (_filter.step(event.value, out)) push<Value>(_next, out);
    }
    void on(Exit const &) {
        ++_counts.exits;
        push<Exit>(_next);
    }
};

// The four filters behind one interface (BankStage's). The bank's own actor forwards.
struct Forward {
    bool step(double value, double &out) noexcept {
        out = value;
        return true;
    }
};
struct DelayStep {
    Delay delay;
    bool  step(double value, double &out) noexcept {
        out = delay.step(value);
        return true;
    }
};
struct FirStep {
    Fir  fir;
    bool step(double value, double &out) noexcept { return fir.step(value, out); }
};
struct SamplerStep {
    Sampler sampler;
    bool    step(double value, double &out) noexcept {
        out = sampler.step(value);
        return true;
    }
};

class Tagger final : public Tallied {
    const std::uint32_t _channel;

public:
    Tagger(const Field &field, std::uint32_t channel, Tally &slot) noexcept
        : Tallied(field, bank_actor(channel, kTagger), slot), _channel(channel) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Value const &event) {
        _counts.add(event.value);
        push<Tagged>(peer(integrator(_field.channels)), _channel, event.value);
    }
    void on(Exit const &) {
        ++_counts.exits;
        push<Exit>(peer(integrator(_field.channels)));
    }
};

class Integrator final : public Tallied {
    Join                _join;
    std::vector<double> _round;

public:
    Integrator(const Field &field, Tally &slot, std::uint64_t rounds)
        : Tallied(field, integrator(field.channels), slot)
        , _join(field.channels, rounds)
        , _round(field.channels) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Tagged>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Tagged const &event) {
        _counts.add(event.value);
        if (!_join.put(event.channel, event.value)) return;
        bool more = true;
        while (more) {
            more = _join.take(_round.data());
            push<Round>(peer(combiner(_field.channels)), _round);
        }
    }
    // One Exit per channel; the last one goes on, behind the last round.
    void on(Exit const &) {
        if (++_counts.exits == _field.channels) push<Exit>(peer(combiner(_field.channels)));
    }
};

class Combine final : public Tallied {
public:
    Combine(const Field &field, Tally &slot) noexcept : Tallied(field, combiner(field.channels), slot) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Round>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Round const &event) {
        _counts.add_round(event.values.data(), _field.channels);
        push<Value>(peer(sink(_field.channels)), combine(event.values.data(), _field.channels));
    }
    void on(Exit const &) {
        ++_counts.exits;
        push<Exit>(peer(sink(_field.channels)));
    }
};

class SinkActor final : public Tallied {
    Sequence    _sequence;
    qvo::Watch &_watch;

public:
    SinkActor(const Field &field, Tally &slot, std::uint64_t *data, std::uint64_t rounds,
              qvo::Watch &watch) noexcept
        : Tallied(field, sink(field.channels), slot), _sequence(data, rounds), _watch(watch) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Value>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(peer(kProducer));
        co_return true;
    }

    void on(Value const &event) {
        _counts.add(event.value);
        _sequence.take(event.value);
    }
    // The Exit follows the last sum through every FIFO: it ends the run.
    void on(Exit const &) {
        ++_counts.exits;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto simulations = static_cast<std::uint64_t>(p.get("simulations"));
    const auto columns     = static_cast<std::uint32_t>(p.get("columns"));
    const auto channels    = static_cast<std::uint32_t>(p.get("channels"));
    const auto cores       = static_cast<int>(p.get("cores"));
    const bool spin        = p.get("wait") != 0;
    const auto nrounds     = rounds(p);

    const Coefficients         coefficients(channels, columns);
    Field                      field;
    std::vector<Tally>         tallies(actors(channels));
    std::vector<std::uint64_t> sequence(static_cast<std::size_t>(nrounds));
    field.channels = channels;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);
        const auto core = [&](std::uint32_t a) {
            return static_cast<qb::CoreId>(core_of(a, channels, ncores));
        };

        field.ids.resize(actors(channels));
        field.ids[kProducer] = engine.addActor<Producer>(core(kProducer), std::cref(field),
                                                         std::ref(tallies[kProducer]), simulations,
                                                         std::ref(watch));
        field.ids[kSource] = engine.addActor<SourceActor>(core(kSource), std::cref(field),
                                                          std::ref(tallies[kSource]));
        field.ids[kBranches] = engine.addActor<Branches>(core(kBranches), std::cref(field),
                                                         std::ref(tallies[kBranches]));
        for (std::uint32_t j = 0; j < channels; ++j) {
            const auto add = [&](Role role, auto filter) {
                const std::uint32_t a = bank_actor(j, role);
                field.ids[a] = engine.addActor<BankStage<decltype(filter)>>(
                    core(a), std::cref(field), a, std::ref(tallies[a]), std::move(filter));
            };
            add(kBank, Forward{});
            add(kDelay1, DelayStep{Delay(columns - 1)});
            add(kFir1, FirStep{Fir(columns, coefficients.h(j))});
            add(kSampler, SamplerStep{Sampler(columns)});
            add(kDelay2, DelayStep{Delay(columns - 1)});
            add(kFir2, FirStep{Fir(columns, coefficients.f(j))});
            const std::uint32_t t = bank_actor(j, kTagger);
            field.ids[t] = engine.addActor<Tagger>(core(t), std::cref(field), j, std::ref(tallies[t]));
        }
        const std::uint32_t i = integrator(channels), c = combiner(channels), s = sink(channels);
        field.ids[i] = engine.addActor<Integrator>(core(i), std::cref(field), std::ref(tallies[i]),
                                                   nrounds);
        field.ids[c] = engine.addActor<Combine>(core(c), std::cref(field), std::ref(tallies[c]));
        field.ids[s] = engine.addActor<SinkActor>(core(s), std::cref(field), std::ref(tallies[s]),
                                                  sequence.data(), nrounds, std::ref(watch));

        engine.start();
        engine.join();
    }
    // The engine is gone: every actor handed its counts to its slot as it was destroyed, and the
    // tallies and the sequence are reduced here, outside the window.
    return qvo::Answer{checksum(tallies, sequence), delivered(tallies)};
}

}  // namespace savina_filterbank_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::filterbank::params();
    spec.expected          = qvospec::savina::filterbank::expected;
    spec.expected_messages = qvospec::savina::filterbank::expected_messages;
    spec.work_unit         = qvospec::savina::filterbank::kWorkUnit;
    spec.work_units        = qvospec::savina::filterbank::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md push<T>() (ordered) + the thread-ring adapter";
    spec.idiom_note        = "the reference's actors one for one, every value and every Exit "
                             "pushed (ordered); a round to the combine as one event; head on core "
                             "0, bank j on core j % cores, the join on the last core";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "the producer, the source and the branches live on core 0, bank j's seven actors on core "
        "j % cores and the integrator, the combine and the sink on the last core "
        "(filterbank::core_of): with cores=2 each core runs four banks' FIRs. CAF and SObjectizer "
        "receive the same budget and their pools place every actor -- see "
        "benchmarks/savina/filterbank.md");

    return qvo::run(argc, argv, std::move(spec), savina_filterbank_qb::body);
}
