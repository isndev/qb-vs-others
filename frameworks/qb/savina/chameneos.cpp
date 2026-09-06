// @benchmark     savina/chameneos
// @framework     qb
// @idiom-source  the big adapter beside this file (one event mutated into its own answer and
//                `reply()`ed, `getSource()` for the sender) and qb/llm/qb.llm.md for `push<>`.
// @idiom-note    A creature's Meet request is answered IN PLACE: the mall turns the event into
//                the meeting announcement (or the exit) and `reply()`s it, so one of the two
//                announcements of a pairing recycles the request that completed it and the
//                other -- to the creature that has been waiting -- is a fresh `push<>`. The
//                mall lives alone on VirtualCore 0 and the creatures on the others, so with
//                cores=2 the mall's inbound queue is one cross-core pipe written by 100 actors
//                on the far side: the fan-in hot spot the spec describes, not a same-core loop.

#include <qvospec/savina/chameneos.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_chameneos_qb {

using namespace qvospec::savina::chameneos;

enum Kind : std::uint8_t { kRequest = 0, kMeeting = 1, kExit = 2 };

// The one hot event: a request going in (colour) and, mutated, the announcement (k, the OTHER
// colour) or the exit coming back.
struct Meet : qb::Event {
    Colour        colour;
    Kind          kind{kRequest};
    std::uint32_t k{0};
    explicit Meet(Colour c) noexcept : colour(c) {}
    Meet(Colour c, Kind kk, std::uint32_t meeting) noexcept : colour(c), kind(kk), k(meeting) {}
};
struct Start : qb::Event {};
struct Ready : qb::Event {};
struct Count : qb::Event {
    std::uint64_t meetings{0};
    std::uint64_t acc{0};
    std::uint64_t received{0};
    Count(std::uint64_t m, std::uint64_t a, std::uint64_t r) noexcept
        : meetings(m), acc(a), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct Field {
    std::vector<qb::ActorId> creatures;
    qb::ActorId              mall;
};

class Creature final : public qb::Actor {
    const Field  &_field;
    Colour        _colour;
    std::uint64_t _meetings{0};
    std::uint64_t _acc{0};
    std::uint64_t _received{0};

public:
    Creature(const Field &field, std::uint32_t index) noexcept
        : _field(field), _colour(initial_colour(index)) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Start>(*this);
        registerEvent<Meet>(*this);
        push<Ready>(_field.mall);
        co_return true;
    }

    void on(Start const &) {
        ++_received;
        push<Meet>(_field.mall, _colour);
    }

    void on(Meet const &event) {
        ++_received;
        if (event.kind == kExit) {
            push<Count>(_field.mall, _meetings, _acc, _received);
            return;
        }
        _colour = complement(_colour, event.colour);
        _acc += qvo::mix(event.k);
        ++_meetings;
        push<Meet>(_field.mall, _colour);
    }
};

class Mall final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _meetings;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::uint32_t       _k{0};
    bool                _waiting{false};
    qb::ActorId         _waiting_id;
    Colour              _waiting_colour{kYellow};
    std::uint64_t       _received{0};
    std::uint64_t       _total_meetings{0};
    std::size_t         _ready{0};
    std::size_t         _counted{0};

public:
    Mall(const Field &field, std::uint32_t meetings, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _meetings(meetings), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Meet>(*this);
        registerEvent<Count>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.creatures.size()) return;
        _watch.start();
        for (const auto &c : _field.creatures) push<Start>(c);
    }

    void on(Meet &event) {
        ++_received;
        if (_k == _meetings) {
            event.kind = kExit;
            reply(event);
            return;
        }
        if (!_waiting) {
            _waiting        = true;
            _waiting_id     = event.getSource();
            _waiting_colour = event.colour;
            return;
        }
        _waiting            = false;
        const Colour theirs = event.colour;
        event.kind          = kMeeting;
        event.k             = _k;
        event.colour        = _waiting_colour;
        reply(event);
        push<Meet>(_waiting_id, theirs, kMeeting, _k);
        ++_k;
    }

    void on(Count const &event) {
        ++_received;
        _sink.checksum += event.acc;
        _sink.messages += event.received;
        _total_meetings += event.meetings;
        if (++_counted != _field.creatures.size()) return;
        _sink.checksum += qvo::mix(_total_meetings);
        _sink.messages += _received;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto creatures = static_cast<std::uint32_t>(p.get("chameneos"));
    const auto meetings  = static_cast<std::uint32_t>(p.get("meetings"));
    const auto cores     = static_cast<int>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.mall = engine.addActor<Mall>(0, std::cref(field), meetings, std::ref(watch),
                                           std::ref(sink));
        field.creatures.reserve(creatures);
        for (std::uint32_t c = 0; c < creatures; ++c) {
            // The mall has core 0 to itself when there is more than one; the creatures share
            // the rest, so every request and every answer crosses a core.
            const int core = ncores == 1 ? 0 : 1 + static_cast<int>(c) % (ncores - 1);
            field.creatures.push_back(engine.addActor<Creature>(static_cast<qb::CoreId>(core),
                                                                std::cref(field), c));
        }

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_chameneos_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::chameneos::params();
    spec.expected          = qvospec::savina::chameneos::expected;
    spec.expected_messages = qvospec::savina::chameneos::expected_messages;
    spec.work_unit         = qvospec::savina::chameneos::kWorkUnit;
    spec.work_units        = qvospec::savina::chameneos::work_units;
    spec.idiom_source      = "the big adapter (reply() recycling, getSource()) + qb/llm/qb.llm.md";
    spec.idiom_note        = "one Meet event per request, mutated into the announcement or the "
                             "exit and reply()ed; the waiting creature gets a fresh push<>; mall "
                             "alone on VirtualCore 0, creatures on the others";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "placement is fixed before start: the mall alone on VirtualCore 0 and the creatures on "
        "the remaining cores (all on core 0 when cores=1), so with cores=2 the mall's mailbox is "
        "one cross-core pipe written by 100 actors on the far side and nothing balances it -- "
        "the pools place the mall and the creatures wherever stealing puts them");

    return qvo::run(argc, argv, std::move(spec), savina_chameneos_qb::body);
}
