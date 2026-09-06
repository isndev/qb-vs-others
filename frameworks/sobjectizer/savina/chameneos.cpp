// @benchmark     savina/chameneos
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the big adapter beside this file (agent_t subclasses on their DIRECT mboxes,
//                messages derived from so_5::message_t, the field's mboxes shared before start)
//                and dev/so_5/disp/thread_pool/pub.hpp via qvoso::make_pool_binder.
// @idiom-note    A request is msg_meet(creature, colour) to the mall's direct mbox, an
//                announcement msg_meeting(k, other) to `field[creature]`, the exit a signal and
//                the report msg_count. Mall and creatures on a thread_pool of `cores` pinned
//                work threads with fifo_t::individual, so which thread runs the mall -- and
//                whether its MPSC queue is written cross-core -- is the dispatcher's decision.

#include <qvospec/savina/chameneos.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_chameneos_sobjectizer {

using namespace qvospec::savina::chameneos;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_meet final : public so_5::message_t {
    std::uint32_t creature;
    Colour        colour;
    msg_meet(std::uint32_t c, Colour col) noexcept : creature(c), colour(col) {}
};
struct msg_meeting final : public so_5::message_t {
    std::uint32_t k;
    Colour        other;
    msg_meeting(std::uint32_t kk, Colour o) noexcept : k(kk), other(o) {}
};
struct msg_exit final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};
struct msg_ready final : public so_5::signal_t {};
struct msg_count final : public so_5::message_t {
    std::uint64_t meetings;
    std::uint64_t acc;
    std::uint64_t received;
    msg_count(std::uint64_t m, std::uint64_t a, std::uint64_t r) noexcept
        : meetings(m), acc(a), received(r) {}
};

struct Field {
    std::vector<so_5::mbox_t> creatures;
    so_5::mbox_t              mall;
};

class creature_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    Colour              m_colour;
    std::uint64_t       m_meetings{0};
    std::uint64_t       m_acc{0};
    std::uint64_t       m_received{0};

    void request() { so_5::send<msg_meet>(m_field.mall, m_index, m_colour); }

public:
    creature_t(context_t ctx, const Field &field, std::uint32_t index)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_index{index}
        , m_colour{initial_colour(index)} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            ++m_received;
            request();
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_meeting> m) {
            ++m_received;
            m_colour = complement(m_colour, m->other);
            m_acc += qvo::mix(m->k);
            ++m_meetings;
            request();
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++m_received;
            so_5::send<msg_count>(m_field.mall, m_meetings, m_acc, m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.mall); }
};

class mall_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_meetings;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    std::uint32_t       m_k{0};
    bool                m_waiting{false};
    std::uint32_t       m_waiting_index{0};
    Colour              m_waiting_colour{kYellow};
    std::uint64_t       m_received{0};
    std::uint64_t       m_total_meetings{0};
    std::size_t         m_ready{0};
    std::size_t         m_counted{0};

public:
    mall_t(context_t ctx, const Field &field, std::uint32_t meetings, qvo::Watch &watch,
           Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_meetings{meetings}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_field.creatures.size()) return;
            m_watch.start();
            for (const auto &c : m_field.creatures) so_5::send<msg_start>(c);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_meet> m) {
            ++m_received;
            if (m_k == m_meetings) {
                so_5::send<msg_exit>(m_field.creatures[m->creature]);
                return;
            }
            if (!m_waiting) {
                m_waiting        = true;
                m_waiting_index  = m->creature;
                m_waiting_colour = m->colour;
                return;
            }
            m_waiting = false;
            so_5::send<msg_meeting>(m_field.creatures[m->creature], m_k, m_waiting_colour);
            so_5::send<msg_meeting>(m_field.creatures[m_waiting_index], m_k, m->colour);
            ++m_k;
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_count> c) {
            ++m_received;
            m_sink.checksum += c->acc;
            m_sink.messages += c->received;
            m_total_meetings += c->meetings;
            if (++m_counted != m_field.creatures.size()) return;
            m_sink.checksum += qvo::mix(m_total_meetings);
            m_sink.messages += m_received;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto creatures = static_cast<std::uint32_t>(p.get("chameneos"));
    const auto meetings  = static_cast<std::uint32_t>(p.get("meetings"));
    const auto cores     = static_cast<int>(p.get("cores"));
    const bool spin      = p.get("wait") != 0;

    Sink  sink;
    Field field;
    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.mall = coop.make_agent<mall_t>(std::cref(field), meetings, std::ref(watch),
                                                 std::ref(sink))
                             ->so_direct_mbox();
            field.creatures.reserve(creatures);
            for (std::uint32_t c = 0; c < creatures; ++c)
                field.creatures.push_back(
                    coop.make_agent<creature_t>(std::cref(field), c)->so_direct_mbox());
        });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_chameneos_sobjectizer

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
    spec.idiom_source      = "the big adapter + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "agents on their DIRECT mboxes, msg_meet / msg_meeting / msg_exit / "
                             "msg_count, the field's mboxes shared before start, "
                             "thread_pool(cores) with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the mall and the 100 creatures are placed by the thread_pool, so whether the mall's "
        "queue is written cross-core is the dispatcher's decision; qb's cell pins the mall alone "
        "on core 0 and every creature on the far side -- see benchmarks/savina/chameneos.md");

    return qvo::run(argc, argv, std::move(spec), savina_chameneos_sobjectizer::body);
}
