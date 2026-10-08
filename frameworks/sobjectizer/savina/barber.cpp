// @benchmark     savina/barber
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the fib adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                thread_pool via qvoso::make_pool_binder) and dev/so_5/environment.hpp's
//                `so_5::introduce_child_coop(*this, binder, lambda)` -- SObjectizer's own way for
//                an agent to create agents at run time -- with
//                `so_deregister_agent_coop_normally()` for a customer's own end.
// @idiom-note    The reference's actors one for one, as agents on their direct mboxes; Savina's
//                payload-free messages are signals. The factory's Start handler loops over every
//                customer exactly as SleepingBarberAkkaActorBenchmark does -- a child coop per
//                customer, Enter to the room, busy-work -- because a SObjectizer send is queued at
//                the receiver at once, so the room starts while the factory is still producing.
//                Every customer is its own child coop of the shop and deregisters it after
//                reporting (an agent ends by deregistering its coop; customers sharing one would
//                take each other down). The factory, the room and the barber are one coop, bound
//                to the pool with individual FIFOs so they run concurrently.

#include <qvospec/savina/barber.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <cstdio>
#include <vector>

namespace savina_barber_sobjectizer {

using namespace qvospec::savina::barber;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t rejections{0};
    std::uint64_t wakeups{0};
};

struct msg_ready final : public so_5::signal_t {};  // handshake, outside the window
struct msg_start final : public so_5::signal_t {};
struct msg_full final : public so_5::signal_t {};
struct msg_wait final : public so_5::signal_t {};
struct msg_next final : public so_5::message_t {
    std::uint64_t number;  // the customer just served; 0 = the room waking the barber
    explicit msg_next(std::uint64_t n) noexcept : number(n) {}
};
struct msg_enter final : public so_5::message_t {
    so_5::mbox_t  customer;
    std::uint64_t number;
    msg_enter(so_5::mbox_t c, std::uint64_t n) noexcept : customer(std::move(c)), number(n) {}
};
struct msg_returned final : public so_5::message_t {
    so_5::mbox_t  customer;
    std::uint64_t number;
    msg_returned(so_5::mbox_t c, std::uint64_t n) noexcept : customer(std::move(c)), number(n) {}
};
struct msg_done final : public so_5::message_t {
    std::uint64_t value;
    std::uint64_t messages;
    msg_done(std::uint64_t v, std::uint64_t m) noexcept : value(v), messages(m) {}
};
struct msg_exit final : public so_5::message_t {
    std::uint64_t partial;
    std::uint64_t messages;
    msg_exit(std::uint64_t p, std::uint64_t m) noexcept : partial(p), messages(m) {}
};

class customer_t final : public so_5::agent_t {
    const so_5::mbox_t  m_factory;
    const std::uint64_t m_number;
    std::uint64_t       m_starts{0};
    std::uint64_t       m_waits{0};
    std::uint64_t       m_fulls{0};
    std::uint64_t       m_received{0};

public:
    customer_t(context_t ctx, so_5::mbox_t factory, std::uint64_t number)
        : so_5::agent_t{std::move(ctx)}, m_factory{std::move(factory)}, m_number{number} {}

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_full>) {
                ++m_received;
                ++m_fulls;
                so_5::send<msg_returned>(m_factory, so_direct_mbox(), m_number);
            })
            .event([this](so_5::mhood_t<msg_wait>) {
                ++m_received;
                ++m_waits;
            })
            .event([this](so_5::mhood_t<msg_start>) {
                ++m_received;
                ++m_starts;
            })
            .event([this](so_5::mhood_t<msg_done> m) {
                ++m_received;
                so_5::send<msg_done>(
                    m_factory,
                    qvo::mix(m_number) + m->value +
                        identity(m_number) * (weight(kTagStart) * m_starts +
                                              weight(kTagWait) * m_waits +
                                              weight(kTagFull) * m_fulls),
                    m_received);
                so_deregister_agent_coop_normally();
            });
    }
};

class factory_t final : public so_5::agent_t {
    const std::uint64_t             m_haircuts;
    const std::uint64_t             m_apr;
    const so_5::disp_binder_shptr_t m_binder;
    qvo::Watch                     &m_watch;
    so_5::mbox_t                    m_room;
    std::uint64_t                   m_ready{0};
    std::uint64_t                   m_served{0};
    std::uint64_t                   m_received{0};
    std::uint64_t                   m_messages{0};  // reported by the customers
    std::uint64_t                   m_sum{0};

public:
    factory_t(context_t ctx, std::uint64_t haircuts, std::uint64_t apr,
              so_5::disp_binder_shptr_t binder, qvo::Watch &watch)
        : so_5::agent_t{std::move(ctx)}
        , m_haircuts{haircuts}
        , m_apr{apr}
        , m_binder{std::move(binder)}
        , m_watch{watch} {}

    void set_room(so_5::mbox_t room) { m_room = std::move(room); }

    void so_define_agent() override {
        so_subscribe_self()
            // The room and the barber are up: open the window with the reference's Start.
            .event([this](so_5::mhood_t<msg_ready>) {
                if (++m_ready != 2) return;
                m_watch.start();
                so_5::send<msg_start>(so_direct_mbox());
            })
            // Start: produce every customer, as the reference does.
            .event([this](so_5::mhood_t<msg_start>) {
                ++m_received;
                for (std::uint64_t i = 0; i < m_haircuts; ++i) {
                    so_5::mbox_t customer;
                    so_5::introduce_child_coop(*this, m_binder, [&](so_5::coop_t &coop) {
                        customer = coop.make_agent<customer_t>(so_direct_mbox(), i + 1)
                                       ->so_direct_mbox();
                    });
                    so_5::send<msg_enter>(m_room, std::move(customer), i + 1);
                    m_sum += production_work(i, m_apr);
                }
            })
            .event([this](so_5::mhood_t<msg_returned> m) {
                ++m_received;
                m_sum += weight(kTagReturned) * identity(m->number);
                so_5::send<msg_enter>(m_room, m->customer, m->number);
            })
            .event([this](so_5::mhood_t<msg_done> m) {
                ++m_received;
                m_sum += m->value;
                m_messages += m->messages;
                if (++m_served == m_haircuts)
                    so_5::send<msg_exit>(m_room, m_sum, m_messages + m_received);
            });
    }
};

class room_t final : public so_5::agent_t {
    struct seat_t {
        so_5::mbox_t  customer;
        std::uint64_t number{0};
    };

    const std::uint64_t m_haircuts;
    Sink               &m_sink;
    so_5::mbox_t        m_factory;
    so_5::mbox_t        m_barber;
    std::vector<seat_t> m_seats;  // a ring of `room` seats
    std::size_t         m_head{0};
    std::size_t         m_waiting{0};
    bool                m_asleep{true};
    std::uint64_t       m_acc{0};  // the room's terms of the checksum (barber.h)
    std::uint64_t       m_rejected{0};
    std::uint64_t       m_wakeups{0};
    std::uint64_t       m_wake_nexts{0};
    std::uint64_t       m_barber_nexts{0};
    std::uint64_t       m_received{0};
    bool                m_exit{false};
    std::uint64_t       m_exit_partial{0};
    std::uint64_t       m_exit_messages{0};

    // Exit leaves only once the barber's n-th Next is in: the factory's Exit and the barber's
    // last Next come from two senders, and nothing orders them (an (n+1)-th fail()s on Next).
    void maybe_exit() {
        if (!m_exit || m_barber_nexts < m_haircuts) return;
        m_exit            = false;
        m_sink.rejections = m_rejected;
        m_sink.wakeups    = m_wakeups;
        const auto partial = m_exit_partial + m_acc + weight(kTagWake) * m_wake_nexts -
                             weight(kTagWake) * m_wakeups;
        so_5::send<msg_exit>(m_barber, partial, m_exit_messages + m_received);
    }

public:
    room_t(context_t ctx, std::uint64_t haircuts, std::uint64_t seats, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_haircuts{haircuts}
        , m_sink{sink}
        , m_seats(static_cast<std::size_t>(seats)) {}

    void wire(so_5::mbox_t factory, so_5::mbox_t barber) {
        m_factory = std::move(factory);
        m_barber  = std::move(barber);
    }

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_enter> m) {
                ++m_received;
                const std::uint64_t id_i = identity(m->number);
                if (m_waiting == m_seats.size()) {
                    ++m_rejected;
                    m_acc -= (weight(kTagFull) + weight(kTagReturned)) * id_i;
                    so_5::send<msg_full>(m->customer);
                    return;
                }
                m_acc += weight(kTagEnter) * id_i;
                m_seats[(m_head + m_waiting++) % m_seats.size()] = seat_t{m->customer, m->number};
                if (m_asleep) {
                    m_asleep = false;
                    ++m_wakeups;
                    m_acc += weight(kTagWait) * id_i;
                    so_5::send<msg_next>(so_direct_mbox(), std::uint64_t{0});
                } else {
                    so_5::send<msg_wait>(m->customer);
                }
            })
            .event([this](so_5::mhood_t<msg_next> m) {
                ++m_received;
                if (m->number == 0) {
                    ++m_wake_nexts;
                } else {
                    if (++m_barber_nexts > m_haircuts)
                        fail("the room received more Nexts than haircuts");
                    m_acc += weight(kTagNext) * identity(m->number);
                }
                if (m_waiting != 0) {
                    seat_t next = std::move(m_seats[m_head]);
                    m_head      = (m_head + 1) % m_seats.size();
                    --m_waiting;
                    so_5::send<msg_enter>(m_barber, std::move(next.customer), next.number);
                } else {
                    m_acc -= weight(kTagNap);
                    m_asleep = true;
                    so_5::send<msg_wait>(m_barber);
                }
                maybe_exit();
            })
            .event([this](so_5::mhood_t<msg_exit> m) {
                ++m_received;
                m_exit          = true;
                m_exit_partial  = m->partial;
                m_exit_messages = m->messages;
                maybe_exit();
            });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_factory); }
};

class barber_t final : public so_5::agent_t {
    const std::uint64_t m_ahr;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    so_5::mbox_t        m_factory;
    so_5::mbox_t        m_room;
    std::uint64_t       m_haircuts{0};
    std::uint64_t       m_acc{0};  // the barber's terms of the checksum (barber.h)
    std::uint64_t       m_received{0};

public:
    barber_t(context_t ctx, std::uint64_t ahr, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_ahr{ahr}, m_watch{watch}, m_sink{sink} {}

    void wire(so_5::mbox_t factory, so_5::mbox_t room) {
        m_factory = std::move(factory);
        m_room    = std::move(room);
    }

    void so_define_agent() override {
        so_subscribe_self()
            .event([this](so_5::mhood_t<msg_enter> m) {
                ++m_received;
                m_acc += weight(kTagCut) * identity(m->number);
                so_5::send<msg_start>(m->customer);
                const std::uint64_t h = haircut_work(m_haircuts++, m_ahr);
                so_5::send<msg_done>(m->customer, h, std::uint64_t{0});
                so_5::send<msg_next>(m_room, m->number);
            })
            .event([this](so_5::mhood_t<msg_wait>) {
                ++m_received;
                m_acc += weight(kTagNap);
            })
            .event([this](so_5::mhood_t<msg_exit> m) {
                ++m_received;
                m_sink.checksum = m->partial + m_acc;
                m_sink.messages = m->messages + m_received;
                m_watch.stop();
                so_environment().stop();
            });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_factory); }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto haircuts = static_cast<std::uint64_t>(p.get("haircuts"));
    const auto seats    = static_cast<std::uint64_t>(p.get("room"));
    const auto apr      = static_cast<std::uint64_t>(p.get("apr"));
    const auto ahr      = static_cast<std::uint64_t>(p.get("ahr"));
    const auto cores    = static_cast<int>(p.get("cores"));
    const bool spin     = p.get("wait") != 0;

    Sink sink;
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            auto *factory = coop.make_agent<factory_t>(haircuts, apr, binder, std::ref(watch));
            auto *room    = coop.make_agent<room_t>(haircuts, seats, std::ref(sink));
            auto *barber  = coop.make_agent<barber_t>(ahr, std::ref(watch), std::ref(sink));
            factory->set_room(room->so_direct_mbox());
            room->wire(factory->so_direct_mbox(), barber->so_direct_mbox());
            barber->wire(factory->so_direct_mbox(), room->so_direct_mbox());
        });
    });
    std::fprintf(stderr, "savina/barber sobjectizer: rejections=%llu wakeups=%llu\n",
                 static_cast<unsigned long long>(sink.rejections),
                 static_cast<unsigned long long>(sink.wakeups));
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_barber_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::barber::params();
    spec.expected          = qvospec::savina::barber::expected;
    spec.work_unit         = qvospec::savina::barber::kWorkUnit;
    spec.work_units        = qvospec::savina::barber::work_units;
    spec.idiom_source      = "the fib adapter + so_5::introduce_child_coop (environment.hpp) + "
                             "so_deregister_agent_coop_normally";
    spec.idiom_note        = "the reference's actors one for one on direct mboxes, payload-free "
                             "messages as signals; the factory's Start handler loops over every "
                             "customer (child coop, Enter, busy-work) as the reference does; "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer creates agents only inside a cooperation registered with the environment, "
        "so every customer pays one child-coop registration and one deregistration on top of the "
        "agent itself -- the framework's own dynamic-agent idiom, as in savina/fib");
    spec.caveats.emplace_back(
        "the factory produces every customer inside ONE handler, as the reference does: a "
        "SObjectizer send is queued at its receiver at once, so the room and the barber start "
        "while the factory is still producing -- qb's and the floor's factories pace themselves "
        "with one Start per customer instead (benchmarks/savina/barber.md)");

    return qvo::run(argc, argv, std::move(spec), savina_barber_sobjectizer::body);
}
