// @benchmark     savina/concsll
// @framework     sobjectizer 5.8.5.1
// @idiom-source  SObjectizer's own samples, shipped in the 5.8.5.1 tree:
//                  dev/sample/so_5/mutable_msg_agents/main.cpp -- a mutable message modified by its
//                    receiver and re-sent with `so_5::send(next, std::move(cmd))`;
//                  dev/sample/so_5/chameneos_prealloc_msgs/main.cpp -- mutable messages allocated
//                    once and reused for every round instead of one message per send;
//                dev/so_5/send_functions.hpp (the redirection overload of `send`, since 5.5.19) and
//                the chameneos / bank-transaction adapters beside this file (agents on their
//                DIRECT mboxes, the field's mboxes shared before start, qvoso::make_pool_binder).
// @idiom-note    form=0, the reference's shape and concdict's -- and SObjectizer's only one (see
//                below): ONE mutable message per worker for the whole run. The worker sends
//                `mutable_msg<msg_op>` to the list's direct mbox, the list writes the answer into
//                it and redirects the same message back to the worker's mbox, and the worker reads
//                the answer, writes its next request into it and redirects it to the list again --
//                SObjectizer's documented zero-allocation form, where a fresh `so_5::send<msg>` per
//                request and per answer would allocate twice per request. SObjectizer has no
//                asynchronous request/reply with a continuation (request_value was removed in 5.6;
//                so_5::extra::async_op is a separate library), so form=1 is not applicable. The
//                list, the master and the workers on a thread_pool of `cores` pinned work threads
//                with fifo_t::individual, so which thread walks the list is the dispatcher's
//                decision.

#include <qvospec/savina/concsll.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_concsll_sobjectizer {

using namespace qvospec::savina::concsll;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    ListStats     stats{};
};

// The request and, on the way back, the answer: `value` is the value to insert or look for going
// out, the list's answer coming back. `worker` is where the list sends it back to.
struct msg_op final : public so_5::message_t {
    std::uint64_t request;  // request_id()
    std::uint32_t kind;
    std::int32_t  value;
    std::uint32_t worker;
    msg_op(std::uint64_t r, std::uint32_t k, std::int32_t v, std::uint32_t w) noexcept
        : request(r), kind(k), value(v), worker(w) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_do_work final : public so_5::signal_t {};
struct msg_end final : public so_5::message_t {
    std::uint64_t acc;
    std::uint64_t received;
    msg_end(std::uint64_t a, std::uint64_t r) noexcept : acc(a), received(r) {}
};
struct msg_finish final : public so_5::signal_t {};
// list -> master, after the window: its list_term (the final contents and the requests it
// received) and what it counted.
struct msg_report final : public so_5::message_t {
    std::uint64_t term;
    std::uint64_t received;
    ListStats     stats;
    msg_report(std::uint64_t t, std::uint64_t r, const ListStats &s) noexcept
        : term(t), received(r), stats(s) {}
};

struct Field {
    so_5::mbox_t              list;
    so_5::mbox_t              master;
    std::vector<so_5::mbox_t> workers;
};

class list_t final : public so_5::agent_t {
    const Field  &m_field;
    SortedList    m_list;
    std::uint64_t m_received{0};
    std::uint64_t m_requests{0};  // the request terms, as the requests arrived

public:
    list_t(context_t ctx, const Field &field) : so_5::agent_t{std::move(ctx)}, m_field{field} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](mutable_mhood_t<msg_op> cmd) {
            ++m_received;
            m_requests += request_term(cmd->request, cmd->kind, cmd->value);
            switch (cmd->kind) {
            case kWrite: m_list.add(cmd->value); break;  // the answer is the value inserted
            case kContains: cmd->value = m_list.contains(cmd->value) ? 1 : 0; break;
            default: cmd->value = m_list.size(); break;
            }
            // The destination is read BEFORE the message is moved into send(): the two arguments
            // of one call are initialised in no specified order.
            const so_5::mbox_t &to = m_field.workers[cmd->worker];
            so_5::send(to, std::move(cmd));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_finish>) {
            so_5::send<msg_report>(m_field.master, list_term(m_list, m_requests), m_received,
                                   m_list.stats());
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class worker_t final : public so_5::agent_t {
    const Field                     &m_field;
    const std::vector<std::uint8_t> &m_written;
    const std::uint32_t              m_index;
    const std::uint64_t              m_messages;
    Script                           m_script;
    Request                          m_asked{};
    std::uint64_t                    m_seq{0};
    std::uint64_t                    m_acc{0};
    std::uint64_t                    m_received{0};

    void finish() { so_5::send<msg_end>(m_field.master, m_acc, m_received); }

public:
    worker_t(context_t ctx, const Field &field, const std::vector<std::uint8_t> &written, std::uint32_t index,
             Config config)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_written{written}
        , m_index{index}
        , m_messages{config.messages}
        , m_script{index, config} {}

    void so_define_agent() override {
        // DoWork: the window is open; the worker's one message is created here.
        so_subscribe_self().event([this](so_5::mhood_t<msg_do_work>) {
            ++m_received;
            if (m_messages == 0) return finish();
            m_asked = m_script.next();
            so_5::send<so_5::mutable_msg<msg_op>>(m_field.list, request_id(m_index, m_seq), m_asked.kind,
                                                  m_asked.value, m_index);
        });
        // The answer to the request in flight, checked against what was asked (answer_term); the
        // next request is written into the same message and leaves from here -- the reference's
        // Worker.process.
        so_subscribe_self().event([this](mutable_mhood_t<msg_op> cmd) {
            ++m_received;
            if (m_seq >= m_messages) fail("a worker answered after its last request");
            m_acc += answer_term(request_id(m_index, m_seq), m_asked, cmd->request, cmd->kind, cmd->value,
                                 m_written);
            if (++m_seq == m_messages) return finish();
            m_asked      = m_script.next();
            cmd->request = request_id(m_index, m_seq);
            cmd->kind    = m_asked.kind;
            cmd->value   = m_asked.value;
            so_5::send(m_field.list, std::move(cmd));
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class master_t final : public so_5::agent_t {
    const Field  &m_field;
    qvo::Watch   &m_watch;
    Sink         &m_sink;
    std::size_t   m_ready{0};
    std::size_t   m_ended{0};
    std::uint64_t m_received{0};

    void close_window() {
        m_watch.stop();
        m_sink.messages += m_received;
        so_5::send<msg_finish>(m_field.list);
    }

public:
    master_t(context_t ctx, const Field &field, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_field{field}, m_watch{watch}, m_sink{sink} {}

    void so_define_agent() override {
        // The list and every worker have started: the window opens.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_field.workers.size() + 1) return;
            m_watch.start();
            if (m_field.workers.empty()) return close_window();
            for (const auto &w : m_field.workers) so_5::send<msg_do_work>(w);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_end> m) {
            ++m_received;
            m_sink.checksum += m->acc;
            m_sink.messages += m->received;
            if (++m_ended == m_field.workers.size()) close_window();
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_report> r) {
            m_sink.checksum += r->term;
            m_sink.messages += r->received;
            m_sink.stats = r->stats;
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = Config::of(p);
    const auto   cores = static_cast<int>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;
    if (asks(p))
        qvo::not_applicable("SObjectizer 5.8 has no asynchronous request/reply with a continuation: "
                            "request_value was removed in 5.6 and so_5::extra::async_op is a "
                            "separate library -- the form=0 cell is SObjectizer's only reply path");

    // Which contains answers are fixed: framework-free, before the window (setup).
    const std::vector<std::uint8_t> written = written_values(c);

    Sink  sink;
    Field field;
    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.master = coop.make_agent<master_t>(std::cref(field), std::ref(watch), std::ref(sink))
                               ->so_direct_mbox();
            field.list = coop.make_agent<list_t>(std::cref(field))->so_direct_mbox();
            field.workers.reserve(c.workers);
            for (std::uint32_t w = 0; w < c.workers; ++w)
                field.workers.push_back(
                    coop.make_agent<worker_t>(std::cref(field), std::cref(written), w, c)->so_direct_mbox());
        });
    });
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed = observations(sink.stats);
    return answer;
}

}  // namespace savina_concsll_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::concsll::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::concsll::expected;
    spec.expected_messages = qvospec::savina::concsll::expected_messages;
    spec.observed_at_least = qvospec::savina::concsll::observed_at_least();
    spec.work_unit         = qvospec::savina::concsll::kWorkUnit;
    spec.work_units        = qvospec::savina::concsll::work_units;
    spec.idiom_source      = "dev/sample/so_5/mutable_msg_agents + chameneos_prealloc_msgs (a mutable "
                             "message modified and re-sent) + dev/so_5/send_functions.hpp";
    spec.idiom_note        = "one mutable_msg<msg_op> per worker for the run, redirected list -> worker -> "
                             "list with so_5::send(mbox, std::move(cmd)); thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "each worker's request and answer travel in ONE mutable message, allocated when the window "
        "opens and redirected back and forth for the whole run (SObjectizer's own "
        "chameneos_prealloc_msgs idiom): no message is allocated per request, where qb writes each "
        "event into its ring and CAF allocates one message per send");
    spec.caveats.emplace_back(
        "the list, the master and the workers are placed by the thread_pool: which thread walks the "
        "list, and whether it changes thread between two requests, is the dispatcher's decision -- qb's "
        "cell pins the list alone on VirtualCore 0, see benchmarks/savina/concsll.md");
    spec.caveats.emplace_back(qvospec::savina::concsll::kSharedListCaveat);

    return qvo::run(argc, argv, std::move(spec), savina_concsll_sobjectizer::body);
}
