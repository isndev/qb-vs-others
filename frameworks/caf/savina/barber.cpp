// @benchmark     savina/barber
// @framework     caf 1.1.0
// @idiom-source  the fib adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms, `self->spawn(...)` from inside a behavior for a child created at
//                run time, `self->quit()` for an actor's own end) -- CAF's own idioms from
//                libcaf_core/caf/scheduled_actor.hpp.
// @idiom-note    The reference's actors one for one, and both factory shapes of the spec's `pace`
//                axis: pace=0 loops over every customer in the Start handler exactly as
//                SleepingBarberAkkaActorBenchmark does -- spawn, mail Enter to the room,
//                busy-work -- and pace=1 produces one customer per self-mailed Start. A mail is
//                enqueued in the room's mailbox at once; whether the room RUNS before the factory's
//                handler returns is the scheduler's decision. Messages are built-in atoms
//                plus arguments, typed per receiver: Start `(tick_atom)`, Enter `(join_atom,
//                actor, uint64 number)`, Full `(leave_atom)`, Wait `(idle_atom)`, Next `(get_atom,
//                uint64 number)`, Returned `(redirect_atom, actor, uint64 number)`, Done
//                `(ok_atom, uint64 value, uint64 messages)`, Exit
//                `(close_atom, uint64 partial, uint64 messages)`. Every customer is spawned by the
//                factory and quits after reporting; the work-stealing pool places everything.

#include <qvospec/savina/barber.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_barber_caf {

using namespace qvospec::savina::barber;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t rejections{0};
    std::uint64_t wakeups{0};
};

struct customer_state {
    caf::actor    factory;
    std::uint64_t number{0};
    std::uint64_t starts{0};
    std::uint64_t waits{0};
    std::uint64_t fulls{0};
    std::uint64_t received{0};
};

struct factory_state {
    caf::actor    room;
    caf::actor    barber;
    std::uint64_t haircuts{0};
    std::uint64_t apr{0};
    bool          paced{false};
    std::uint64_t produced{0};
    std::uint64_t ready{0};
    std::uint64_t served{0};
    std::uint64_t received{0};
    std::uint64_t messages{0};  // reported by the customers
    std::uint64_t sum{0};
    qvo::Watch   *watch{nullptr};
};

struct seat {
    caf::actor    customer;
    std::uint64_t number{0};
};

struct room_state {
    caf::actor        factory;
    caf::actor        barber;
    std::vector<seat> seats;  // a ring of `room` seats
    std::size_t       head{0};
    std::size_t       waiting{0};
    bool              asleep{true};
    std::uint64_t     haircuts{0};
    std::uint64_t     acc{0};  // the room's terms of the checksum (barber.h)
    std::uint64_t     rejected{0};
    std::uint64_t     wakeups{0};
    std::uint64_t     wake_nexts{0};
    std::uint64_t     barber_nexts{0};
    std::uint64_t     received{0};
    bool                    exit{false};
    std::uint64_t           exit_partial{0};
    std::uint64_t           exit_messages{0};
    Sink                   *sink{nullptr};
};

struct barber_state {
    caf::actor    factory;
    caf::actor    room;
    std::uint64_t ahr{0};
    std::uint64_t haircuts{0};
    std::uint64_t acc{0};  // the barber's terms of the checksum (barber.h)
    std::uint64_t received{0};
    qvo::Watch   *watch{nullptr};
    Sink         *sink{nullptr};
};

caf::behavior customer_fun(caf::stateful_actor<customer_state> *self, caf::actor factory,
                           std::uint64_t number) {
    self->state().factory = std::move(factory);
    self->state().number  = number;
    return {
        [self](caf::leave_atom) {  // Full
            auto &s = self->state();
            ++s.received;
            ++s.fulls;
            self->mail(caf::redirect_atom_v, caf::actor_cast<caf::actor>(self), s.number)
                .send(s.factory);
        },
        [self](caf::idle_atom) {  // Wait
            auto &s = self->state();
            ++s.received;
            ++s.waits;
        },
        [self](caf::tick_atom) {  // Start
            auto &s = self->state();
            ++s.received;
            ++s.starts;
        },
        [self](caf::ok_atom, std::uint64_t haircut, std::uint64_t) {  // Done
            auto &s = self->state();
            ++s.received;
            self->mail(caf::ok_atom_v,
                       qvo::mix(s.number) + haircut +
                           identity(s.number) * (weight(kTagStart) * s.starts +
                                                 weight(kTagWait) * s.waits +
                                                 weight(kTagFull) * s.fulls),
                       s.received)
                .send(s.factory);
            self->quit();
        },
    };
}

caf::behavior barber_fun(caf::stateful_actor<barber_state> *self, caf::actor factory,
                         std::uint64_t ahr, qvo::Watch *watch, Sink *sink) {
    auto &st   = self->state();
    st.factory = std::move(factory);
    st.ahr     = ahr;
    st.watch   = watch;
    st.sink    = sink;
    return {
        // Handshake, once, outside the window: the barber learns his room and says he is up.
        [self](caf::contact_atom, caf::actor room) {
            auto &s = self->state();
            s.room  = std::move(room);
            self->mail(caf::ok_atom_v).send(s.factory);
        },
        [self](caf::join_atom, caf::actor customer, std::uint64_t number) {  // Enter: serve one
            auto &s = self->state();
            ++s.received;
            s.acc += weight(kTagCut) * identity(number);
            self->mail(caf::tick_atom_v).send(customer);
            const std::uint64_t h = haircut_work(s.haircuts++, s.ahr);
            self->mail(caf::ok_atom_v, h, std::uint64_t{0}).send(customer);
            self->mail(caf::get_atom_v, number).send(s.room);
        },
        [self](caf::idle_atom) {  // Wait: nobody in the room
            auto &s = self->state();
            ++s.received;
            s.acc += weight(kTagNap);
        },
        [self](caf::close_atom, std::uint64_t partial, std::uint64_t messages) {  // Exit
            auto &s = self->state();
            ++s.received;
            s.sink->checksum = partial + s.acc;
            s.sink->messages = messages + s.received;
            s.watch->stop();
            self->quit();
        },
    };
}

caf::behavior room_fun(caf::stateful_actor<room_state> *self, caf::actor factory,
                       caf::actor barber, std::uint64_t haircuts, std::uint64_t seats,
                       Sink *sink) {
    auto &st    = self->state();
    st.factory  = std::move(factory);
    st.barber   = std::move(barber);
    st.haircuts = haircuts;
    st.sink     = sink;
    st.seats.resize(static_cast<std::size_t>(seats));

    // Exit leaves only once the barber's n-th Next is in: the factory's Exit and the barber's last
    // Next come from two senders, and nothing orders them (an (n+1)-th fail()s on Next).
    auto maybe_exit = [self] {
        auto &s = self->state();
        if (!s.exit || s.barber_nexts < s.haircuts) return;
        s.exit             = false;
        s.sink->rejections = s.rejected;
        s.sink->wakeups    = s.wakeups;
        const auto partial =
            s.exit_partial + s.acc + weight(kTagWake) * s.wake_nexts - weight(kTagWake) * s.wakeups;
        self->mail(caf::close_atom_v, partial, s.exit_messages + s.received).send(s.barber);
        self->quit();
    };

    return {
        // Handshake, once, outside the window.
        [self](caf::tick_atom) { self->mail(caf::ok_atom_v).send(self->state().factory); },
        [self](caf::join_atom, caf::actor customer, std::uint64_t number) {  // Enter
            auto               &s    = self->state();
            const std::uint64_t id_i = identity(number);
            ++s.received;
            if (s.waiting == s.seats.size()) {
                ++s.rejected;
                s.acc -= (weight(kTagFull) + weight(kTagReturned)) * id_i;
                self->mail(caf::leave_atom_v).send(customer);
                return;
            }
            s.acc += weight(kTagEnter) * id_i;
            if (s.asleep) {
                s.asleep = false;
                ++s.wakeups;
                s.acc += weight(kTagWait) * id_i;
                self->mail(caf::get_atom_v, std::uint64_t{0})
                    .send(caf::actor_cast<caf::actor>(self));
            } else {
                self->mail(caf::idle_atom_v).send(customer);
            }
            s.seats[(s.head + s.waiting++) % s.seats.size()] = seat{std::move(customer), number};
        },
        [self, maybe_exit](caf::get_atom, std::uint64_t number) {  // Next
            auto &s = self->state();
            ++s.received;
            if (number == 0) {
                ++s.wake_nexts;
            } else {
                if (++s.barber_nexts > s.haircuts) fail("the room received more Nexts than haircuts");
                s.acc += weight(kTagNext) * identity(number);
            }
            if (s.waiting != 0) {
                seat next = std::move(s.seats[s.head]);
                s.head    = (s.head + 1) % s.seats.size();
                --s.waiting;
                self->mail(caf::join_atom_v, std::move(next.customer), next.number).send(s.barber);
            } else {
                s.acc -= weight(kTagNap);
                s.asleep = true;
                self->mail(caf::idle_atom_v).send(s.barber);
            }
            maybe_exit();
        },
        [self, maybe_exit](caf::close_atom, std::uint64_t partial, std::uint64_t messages) {
            auto &s         = self->state();
            ++s.received;
            s.exit          = true;
            s.exit_partial  = partial;
            s.exit_messages = messages;
            maybe_exit();
        },
    };
}

caf::behavior factory_fun(caf::stateful_actor<factory_state> *self, std::uint64_t haircuts,
                          std::uint64_t seats, std::uint64_t apr, std::uint64_t ahr, bool paced,
                          qvo::Watch *watch, Sink *sink) {
    auto &st    = self->state();
    st.haircuts = haircuts;
    st.apr      = apr;
    st.paced    = paced;
    st.watch    = watch;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.barber     = self->spawn<qvocaf::kSpawnOptions>(barber_fun, me, ahr, watch, sink);
    st.room = self->spawn<qvocaf::kSpawnOptions>(room_fun, me, st.barber, haircuts, seats, sink);
    self->mail(caf::contact_atom_v, st.room).send(st.barber);
    self->mail(caf::tick_atom_v).send(st.room);

    return {
        // The room and the barber are up and scheduled: open the window with the reference's
        // Start.
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != 2) return;
            s.watch->start();
            self->mail(caf::tick_atom_v).send(caf::actor_cast<caf::actor>(self));
        },
        // Start. pace=0: every customer from this one handler, as the reference; pace=1: one.
        [self](caf::tick_atom) {
            auto      &s  = self->state();
            const auto me = caf::actor_cast<caf::actor>(self);
            ++s.received;
            do {
                const std::uint64_t i        = s.produced++;
                auto                customer = self->spawn<qvocaf::kSpawnOptions>(customer_fun, me, i + 1);
                self->mail(caf::join_atom_v, std::move(customer), i + 1).send(s.room);
                s.sum += production_work(i, s.apr);
            } while (!s.paced && s.produced < s.haircuts);
            if (s.paced && s.produced < s.haircuts) self->mail(caf::tick_atom_v).send(me);
        },
        [self](caf::redirect_atom, caf::actor customer, std::uint64_t number) {  // Returned
            auto &s = self->state();
            ++s.received;
            s.sum += weight(kTagReturned) * identity(number);
            self->mail(caf::join_atom_v, std::move(customer), number).send(s.room);
        },
        [self](caf::ok_atom, std::uint64_t value, std::uint64_t messages) {  // Done
            auto &s = self->state();
            ++s.received;
            s.sum += value;
            s.messages += messages;
            if (++s.served != s.haircuts) return;
            self->mail(caf::close_atom_v, s.sum, s.messages + s.received).send(s.room);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto haircuts = static_cast<std::uint64_t>(p.get("haircuts"));
    const auto seats    = static_cast<std::uint64_t>(p.get("room"));
    const auto apr      = static_cast<std::uint64_t>(p.get("apr"));
    const auto ahr      = static_cast<std::uint64_t>(p.get("ahr"));
    const bool pace     = paced(p);
    const auto cores    = static_cast<std::size_t>(p.get("cores"));
    const bool spin     = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(factory_fun, haircuts, seats, apr, ahr, pace, &watch,
                                         &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedRejections] = sink.rejections;
    answer.observed[kObservedWakeups]    = sink.wakeups;
    return answer;
}

}  // namespace savina_barber_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::barber::params();
    // The factory shape this adapter's table cell runs: the reference's (pace=0) until the
    // quiet-host measurement of both forms names the faster one (benchmarks/savina/barber.md).
    spec.params["pace"] = 0;
    spec.expected          = qvospec::savina::barber::expected;
    spec.work_unit         = qvospec::savina::barber::kWorkUnit;
    spec.work_units        = qvospec::savina::barber::work_units;
    spec.observed_at_least[qvospec::savina::barber::kObservedWakeups] =
        qvospec::savina::barber::min_wakeups;
    spec.idiom_source      = "the fib adapter + self->spawn() from a behavior (scheduled_actor.hpp)";
    spec.idiom_note        = "the reference's actors one for one; pace=0: the Start handler loops "
                             "over every customer (spawn, mail Enter, busy-work) as the reference "
                             "does, pace=1: one customer per self-mailed Start; built-in atoms "
                             "typed per receiver; placement left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the run's `pace` is in its params: 0 is the reference's factory, every customer from one "
        "handler; 1 adds n-1 self-mailed Starts to the reported count. A mail is enqueued in the "
        "room's mailbox at once, but the pool decides when the room runs: on the unpinned "
        "correctness runs at cores=2 with pace=0, CAF woke the barber exactly once -- the room "
        "did not run until production had ended (benchmarks/savina/barber.md)");

    return qvo::run(argc, argv, std::move(spec), savina_barber_caf::body);
}
