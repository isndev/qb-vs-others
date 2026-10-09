// @benchmark     savina/cigsmok
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md -- `reply(e)` (the received event, its destination and source
//                swapped, sent back: no new event is built) and "`send<T>()` is unordered;
//                `push<T>()` is ordered" (`send` hands the event to the peer's ring at once
//                instead of the pass's batched flush, which wins for ONE event with nothing behind
//                it to batch, a request the sender then idles for) -- plus the chameneos adapter
//                beside this file (an event answered in place with `reply()`).
// @idiom-note    The arbiter's StartSmoking is a `send<Smoke>`: one event per decision, and the
//                arbiter idles until it is acknowledged. The smoker acknowledges by `reply()`ing
//                the very event it received -- Savina's StartedSmoking -- BEFORE it smokes, and
//                `reply` goes through `send`: the acknowledgement is in the arbiter's ring while
//                the smoke runs, which is the overlap the reference's order of statements exists
//                for (a `push` would be published by the pass's flush, after the smoke). The end
//                is a `push<Exit>` per smoker -- 200 events in one handler, the batch `push` is
//                for -- and a `push<Report>` back. Every actor is placed before start with
//                `Main::addActor`, the one way to put an actor on another VirtualCore: the arbiter
//                on core 0, smoker j on core (j + 1) % cores.

#include <qvospec/savina/cigsmok.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <cstdio>
#include <vector>

namespace savina_cigsmok_qb {

using namespace qvospec::savina::cigsmok;

struct Ready : qb::Event {};  // handshake, outside the window
struct Start : qb::Event {};
// StartSmoking going out (the round, its period, the smoker it was drawn for) and, replied by the
// smoker unchanged, StartedSmoking coming back.
struct Smoke : qb::Event {
    std::uint64_t round{0};
    std::uint32_t period{0};
    std::uint32_t smoker{0};
    Smoke(std::uint64_t r, std::uint32_t p, std::uint32_t s) noexcept
        : round(r), period(p), smoker(s) {}
};
struct Exit : qb::Event {};
struct Report : qb::Event {
    std::uint64_t partial{0};
    std::uint64_t messages{0};
    Report(std::uint64_t p, std::uint64_t m) noexcept : partial(p), messages(m) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t stale{0};
};

// The ids, filled before the engine starts (ids are assigned at addActor time, the actors are
// constructed at start) and read-only from then on.
struct Field {
    qb::ActorId              arbiter;
    std::vector<qb::ActorId> smokers;
};

class Smoker final : public qb::Actor {
    const qb::ActorId   _arbiter;
    const std::uint32_t _number;
    std::uint64_t       _acc{0};
    std::uint64_t       _received{0};

public:
    Smoker(qb::ActorId arbiter, std::uint32_t number) noexcept
        : _arbiter(arbiter), _number(number) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Smoke>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(_arbiter);
        co_return true;
    }

    // StartSmoking: acknowledge first -- the ingredients are off the table -- then smoke, as the
    // reference does. The acknowledgement is the received event itself, replied.
    void on(Smoke &event) {
        ++_received;
        const std::uint64_t round  = event.round;
        const std::uint32_t period = event.period;
        reply(event);
        _acc += smoke_term(_number, round, period);
    }

    void on(Exit const &) {
        ++_received;
        _acc += exit_term(_number);
        push<Report>(_arbiter, _acc, _received);
        kill();
    }
};

class Arbiter final : public qb::Actor {
    const Field        &_field;
    const std::uint64_t _rounds;
    const std::uint64_t _smoke;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::size_t         _ready{0};
    std::uint64_t       _outstanding{0};  // the round whose StartedSmoking is awaited
    std::uint64_t       _played{0};       // rounds acknowledged
    bool                _exiting{false};
    std::size_t         _reports{0};
    std::uint64_t       _acc{0};          // the arbiter's terms of the checksum (cigsmok.h)
    std::uint64_t       _messages{0};     // reported by the smokers
    std::uint64_t       _received{0};
    std::uint64_t       _stale{0};

    // Put the ingredients on the table for round `round`: the draw names the smoker and the period.
    void choose(std::uint64_t round) {
        const std::uint32_t smoker = smoker_of(round, _field.smokers.size());
        _outstanding               = round;
        send<Smoke>(_field.smokers[smoker], round, period_of(round, _smoke), smoker);
    }

public:
    Arbiter(const Field &field, std::uint64_t rounds, std::uint64_t smoke, qvo::Watch &watch,
            Sink &sink) noexcept
        : _field(field), _rounds(rounds), _smoke(smoke), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Start>(*this);
        registerEvent<Smoke>(*this);
        registerEvent<Report>(*this);
        co_return true;
    }

    // Every smoker is up and scheduled: open the window with the reference's StartMessage.
    void on(Ready const &) {
        if (++_ready != _field.smokers.size()) return;
        _watch.start();
        push<Start>(id());
    }

    void on(Start const &) {
        ++_received;
        choose(0);
    }

    // StartedSmoking. Every one is folded into the sum; only the one naming the outstanding round
    // plays the next -- a duplicate completes the run with a wrong sum instead of a second round.
    void on(Smoke const &event) {
        ++_received;
        _acc += ack_term(event.smoker, event.round);
        if (_exiting || event.round != _outstanding) {
            ++_stale;
            return;
        }
        if (++_played < _rounds) {
            choose(_played);
            return;
        }
        _exiting = true;
        for (const auto &smoker : _field.smokers) push<Exit>(smoker);
    }

    void on(Report const &event) {
        ++_received;
        _acc += event.partial;
        _messages += event.messages;
        if (++_reports != _field.smokers.size()) return;
        _sink.checksum = _acc;
        _sink.messages = _messages + _received;
        _sink.stale    = _stale;
        _watch.stop();
        kill();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto rounds  = at_least_one(p.get("rounds"), "rounds");
    const auto smokers = at_least_one(p.get("smokers"), "smokers");
    const auto smoke   = at_least_one(p.get("smoke"), "smoke");
    const auto cores   = static_cast<int>(p.get("cores"));
    const bool spin    = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.arbiter = engine.addActor<Arbiter>(0, std::cref(field), rounds, smoke,
                                                 std::ref(watch), std::ref(sink));
        field.smokers.reserve(static_cast<std::size_t>(smokers));
        for (std::uint32_t j = 0; j < smokers; ++j) {
            // Smoker j on core (j + 1) % cores: with two cores, half the smokes run beside the
            // arbiter and overlap its next decision, half share its core.
            const auto core = static_cast<qb::CoreId>((j + 1) % static_cast<std::uint32_t>(ncores));
            field.smokers.push_back(engine.addActor<Smoker>(core, field.arbiter, j));
        }

        engine.start();
        engine.join();
    }
    if (sink.stale != 0)
        std::fprintf(stderr, "savina/cigsmok qb: %llu StartedSmoking named no outstanding round\n",
                     static_cast<unsigned long long>(sink.stale));
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_cigsmok_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::cigsmok::params();
    spec.expected          = qvospec::savina::cigsmok::expected;
    spec.expected_messages = qvospec::savina::cigsmok::expected_messages;
    spec.work_unit         = qvospec::savina::cigsmok::kWorkUnit;
    spec.work_units        = qvospec::savina::cigsmok::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: reply(e) (the received event sent back) + "
                             "send<T>() (into the peer's ring at once, not at the pass's flush) "
                             "+ the chameneos adapter";
    spec.idiom_note        = "StartSmoking is a send<Smoke> per decision; the smoker reply()s "
                             "the same event as StartedSmoking BEFORE it smokes, so the "
                             "acknowledgement is published while the smoke runs; Exit and Report "
                             "are push<>; arbiter on VirtualCore 0, smoker j on core "
                             "(j + 1) % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "placement is fixed before start: the arbiter on VirtualCore 0 and smoker j on core "
        "(j + 1) % cores, so with cores=2 half the smokers share the arbiter's core. A smoke on "
        "the far core overlaps the arbiter's next decision; a smoke on the arbiter's core holds "
        "the arbiter until it ends (its acknowledgement waits on the same thread). The pools place "
        "the arbiter and the smokers wherever stealing puts them");
    spec.caveats.emplace_back(
        "the acknowledgement is the received StartSmoking reply()ed, which goes through send<>: "
        "published into the arbiter's core at once, before the smoke, where a push<> would be "
        "published by the pass's flush after it -- benchmarks/savina/cigsmok.md");

    return qvo::run(argc, argv, std::move(spec), savina_cigsmok_qb::body);
}
