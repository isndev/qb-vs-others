// @benchmark     savina/barber
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md -- "VirtualCore" (per pass: drain the inter-core mailbox, drain
//                the local queue, flush the outgoing pipes) and "Dynamic actor creation"
//                (`addRefActor<T>()` from inside an actor creates a child ON THE SAME VirtualCore,
//                its `onInit` run synchronously, the handle's `id()` valid before the call
//                returns) -- plus `kill()` for a customer's own end.
// @idiom-note    The factory paces itself: its Start handler creates ONE customer, pushes it to the
//                room, busy-works the production delay and pushes Start to itself again, where
//                Savina's factory loops over every customer inside one handler. In qb a push to
//                another VirtualCore is published by the pass's flush, after the handler returns,
//                so a factory that looped would hold all its customers until production ended
//                and the room would never overlap with it; a short handler per item is how qb
//                writes a producer. The price is n-1 self-addressed Starts the reference does not
//                send, counted in the reported messages. Every customer is an actor created by the
//                factory with `addRefActor` -- on the factory's core -- and killed after it reports.
//                With cores=2 the room and the barber share the other core, so the barber's
//                Next -> Enter turn never crosses a core.

#include <qvospec/savina/barber.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace savina_barber_qb {

using namespace qvospec::savina::barber;

// Savina's message set, one type each; like the reference, Start, Enter, Wait and Done mean
// slightly different things to different receivers (the factory's Start is the kick, a customer's
// is the haircut starting; the room's Enter is an arrival, the barber's a customer to cut).
struct Ready : qb::Event {};  // handshake, outside the window
struct Start : qb::Event {};
struct Enter : qb::Event {
    qb::ActorId   customer;
    std::uint64_t number{0};
    Enter(qb::ActorId c, std::uint64_t n) noexcept : customer(c), number(n) {}
};
struct Full : qb::Event {};
struct Wait : qb::Event {};
struct Next : qb::Event {
    std::uint64_t number{0};  // the customer just served; 0 = the room waking the barber
    explicit Next(std::uint64_t n) noexcept : number(n) {}
};
struct Returned : qb::Event {  // the customer is the event's source
    std::uint64_t number{0};
    explicit Returned(std::uint64_t n) noexcept : number(n) {}
};
struct Done : qb::Event {
    std::uint64_t value{0};
    std::uint64_t messages{0};
    Done(std::uint64_t v, std::uint64_t m) noexcept : value(v), messages(m) {}
};
struct Exit : qb::Event {
    std::uint64_t partial{0};
    std::uint64_t messages{0};
    Exit(std::uint64_t p, std::uint64_t m) noexcept : partial(p), messages(m) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t rejections{0};
    std::uint64_t wakeups{0};
};

// The three ids, filled before the engine starts (ids are assigned at addActor time, the actors
// are constructed at start) and read-only from then on.
struct Field {
    qb::ActorId factory;
    qb::ActorId room;
    qb::ActorId barber;
};

class Customer final : public qb::Actor {
    const qb::ActorId   _factory;
    const std::uint64_t _number;
    std::uint64_t       _starts{0};
    std::uint64_t       _waits{0};
    std::uint64_t       _fulls{0};
    std::uint64_t       _received{0};

public:
    Customer(qb::ActorId factory, std::uint64_t number) noexcept
        : _factory(factory), _number(number) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Full>(*this);
        registerEvent<Wait>(*this);
        registerEvent<Start>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    void on(Full const &) {
        ++_received;
        ++_fulls;
        push<Returned>(_factory, _number);
    }
    void on(Wait const &) {
        ++_received;
        ++_waits;
    }
    void on(Start const &) {
        ++_received;
        ++_starts;
    }
    void on(Done const &event) {
        ++_received;
        push<Done>(_factory,
                   qvo::mix(_number) + event.value +
                       identity(_number) * (weight(kTagStart) * _starts +
                                            weight(kTagWait) * _waits + weight(kTagFull) * _fulls),
                   _received);
        kill();
    }
};

class Factory final : public qb::Actor {
    const Field        &_field;
    const std::uint64_t _haircuts;
    const std::uint64_t _apr;
    qvo::Watch         &_watch;
    std::uint64_t       _ready{0};
    std::uint64_t       _produced{0};
    std::uint64_t       _served{0};
    std::uint64_t       _received{0};
    std::uint64_t       _messages{0};  // reported by the customers
    std::uint64_t       _sum{0};

public:
    Factory(const Field &field, std::uint64_t haircuts, std::uint64_t apr,
            qvo::Watch &watch) noexcept
        : _field(field), _haircuts(haircuts), _apr(apr), _watch(watch) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Start>(*this);
        registerEvent<Returned>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    // The room and the barber are up and scheduled: open the window with the reference's Start.
    void on(Ready const &) {
        if (++_ready != 2) return;
        _watch.start();
        push<Start>(id());
    }

    void on(Start const &) {
        ++_received;
        const auto customer = addRefActor<Customer>(id(), _produced + 1);
        if (!customer.valid()) {
            std::fprintf(stderr, "savina/barber qb: addRefActor returned an invalid handle -- the "
                                 "VirtualCore's actor id pool is exhausted\n");
            std::abort();
        }
        push<Enter>(_field.room, customer.id(), _produced + 1);
        _sum += production_work(_produced, _apr);
        if (++_produced < _haircuts) push<Start>(id());
    }

    void on(Returned const &event) {
        ++_received;
        _sum += weight(kTagReturned) * identity(event.number);
        push<Enter>(_field.room, event.getSource(), event.number);
    }

    void on(Done const &event) {
        ++_received;
        _sum += event.value;
        _messages += event.messages;
        if (++_served == _haircuts)
            push<Exit>(_field.room, _sum, _messages + _received);
    }
};

class Room final : public qb::Actor {
    struct Seat {
        qb::ActorId   customer;
        std::uint64_t number{0};
    };

    const Field        &_field;
    const std::uint64_t _haircuts;
    Sink               &_sink;
    std::vector<Seat>   _seats;  // a ring of `room` seats
    std::size_t         _head{0};
    std::size_t         _waiting{0};
    bool                _asleep{true};
    std::uint64_t       _acc{0};  // the room's terms of the checksum (barber.h)
    std::uint64_t       _rejected{0};
    std::uint64_t       _wakeups{0};
    std::uint64_t       _wake_nexts{0};
    std::uint64_t       _barber_nexts{0};
    std::uint64_t       _received{0};
    bool                _exit{false};
    std::uint64_t       _exit_partial{0};
    std::uint64_t       _exit_messages{0};

    // Exit leaves only once the barber's n-th Next is in: the factory's Exit and the barber's
    // last Next come from two senders, and nothing orders them (an (n+1)-th fail()s in on(Next)).
    void maybe_exit() {
        if (!_exit || _barber_nexts < _haircuts) return;
        _exit            = false;
        _sink.rejections = _rejected;
        _sink.wakeups    = _wakeups;
        const auto partial =
            _exit_partial + _acc + weight(kTagWake) * _wake_nexts - weight(kTagWake) * _wakeups;
        push<Exit>(_field.barber, partial, _exit_messages + _received);
    }

public:
    Room(const Field &field, std::uint64_t haircuts, std::uint64_t seats, Sink &sink)
        : _field(field), _haircuts(haircuts), _sink(sink), _seats(seats) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Enter>(*this);
        registerEvent<Next>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(_field.factory);
        co_return true;
    }

    void on(Enter const &event) {
        ++_received;
        const std::uint64_t id_i = identity(event.number);
        if (_waiting == _seats.size()) {
            ++_rejected;
            _acc -= (weight(kTagFull) + weight(kTagReturned)) * id_i;
            push<Full>(event.customer);
            return;
        }
        _acc += weight(kTagEnter) * id_i;
        _seats[(_head + _waiting++) % _seats.size()] = Seat{event.customer, event.number};
        if (_asleep) {
            _asleep = false;
            ++_wakeups;
            _acc += weight(kTagWait) * id_i;
            push<Next>(id(), std::uint64_t{0});
        } else {
            push<Wait>(event.customer);
        }
    }

    void on(Next const &event) {
        ++_received;
        if (event.number == 0) {
            ++_wake_nexts;
        } else {
            if (++_barber_nexts > _haircuts) fail("the room received more Nexts than haircuts");
            _acc += weight(kTagNext) * identity(event.number);
        }
        if (_waiting != 0) {
            const Seat seat = _seats[_head];
            _head           = (_head + 1) % _seats.size();
            --_waiting;
            push<Enter>(_field.barber, seat.customer, seat.number);
        } else {
            _acc -= weight(kTagNap);
            push<Wait>(_field.barber);
            _asleep = true;
        }
        maybe_exit();
    }

    void on(Exit const &event) {
        ++_received;
        _exit          = true;
        _exit_partial  = event.partial;
        _exit_messages = event.messages;
        maybe_exit();
    }
};

class Barber final : public qb::Actor {
    const Field        &_field;
    const std::uint64_t _ahr;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::uint64_t       _haircuts{0};
    std::uint64_t       _acc{0};  // the barber's terms of the checksum (barber.h)
    std::uint64_t       _received{0};

public:
    Barber(const Field &field, std::uint64_t ahr, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _ahr(ahr), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Enter>(*this);
        registerEvent<Wait>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(_field.factory);
        co_return true;
    }

    void on(Enter const &event) {
        ++_received;
        _acc += weight(kTagCut) * identity(event.number);
        push<Start>(event.customer);
        const std::uint64_t h = haircut_work(_haircuts++, _ahr);
        push<Done>(event.customer, h, std::uint64_t{0});
        push<Next>(_field.room, event.number);
    }

    void on(Wait const &) {
        ++_received;
        _acc += weight(kTagNap);
    }

    void on(Exit const &event) {
        ++_received;
        _sink.checksum = event.partial + _acc;
        _sink.messages = event.messages + _received;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto haircuts = static_cast<std::uint64_t>(p.get("haircuts"));
    const auto seats    = static_cast<std::uint64_t>(p.get("room"));
    const auto apr      = static_cast<std::uint64_t>(p.get("apr"));
    const auto ahr      = static_cast<std::uint64_t>(p.get("ahr"));
    const auto cores    = static_cast<int>(p.get("cores"));
    const bool spin     = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);
        const auto shop = static_cast<qb::CoreId>(1 % ncores);

        field.factory = engine.addActor<Factory>(0, std::cref(field), haircuts, apr, std::ref(watch));
        field.room    = engine.addActor<Room>(shop, std::cref(field), haircuts, seats, std::ref(sink));
        field.barber  = engine.addActor<Barber>(shop, std::cref(field), ahr, std::ref(watch),
                                                std::ref(sink));

        engine.start();
        engine.join();
    }
    std::fprintf(stderr, "savina/barber qb: rejections=%llu wakeups=%llu\n",
                 static_cast<unsigned long long>(sink.rejections),
                 static_cast<unsigned long long>(sink.wakeups));
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_barber_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::barber::params();
    spec.expected          = qvospec::savina::barber::expected;
    spec.work_unit         = qvospec::savina::barber::kWorkUnit;
    spec.work_units        = qvospec::savina::barber::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: VirtualCore pass (flush after the handlers) + "
                             "addRefActor<T>() (same-core child, onInit run synchronously) + kill()";
    spec.idiom_note        = "the factory creates one customer per self-addressed Start (n-1 more "
                             "messages than the reference); customers are addRefActor children of "
                             "the factory, killed after reporting; room and barber share a core";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "the factory paces itself with one Start per customer, where the reference loops over "
        "every customer in one handler: a qb push to another core is published by the pass's "
        "flush, after the handler returns, so a looping factory would release its customers only "
        "once production ended -- qb's idiom is a short handler, and the n-1 extra self-addressed "
        "messages are in the reported count");
    spec.caveats.emplace_back(
        "with cores=2 the factory and every customer live on core 0 (a customer is created on its "
        "creator's core) and the room and the barber on core 1, so the barber's Next -> Enter turn "
        "stays on his core and production overlaps haircuts -- the static placement the floor "
        "has; the pools place by themselves");

    return qvo::run(argc, argv, std::move(spec), savina_barber_qb::body);
}
