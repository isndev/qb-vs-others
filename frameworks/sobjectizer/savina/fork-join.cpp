// @benchmark     savina/fork-join
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the ping-pong and counting adapters beside this file (agent_t subclasses on
//                their DIRECT mboxes, messages derived from so_5::message_t) and dev/so_5/disp/
//                thread_pool/pub.hpp for the many-agent dispatcher -- qvoso::make_pool_binder.
// @idiom-note    Sixty workers on a thread_pool of `cores` pinned work threads with
//                fifo_t::individual, so two workers' demands can run concurrently. The master's
//                loop is counting's producer loop over sixty direct mboxes; a job is one
//                message_t carrying its index.

#include <qvospec/savina/fork-join.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_fork_join_sobjectizer {

using namespace qvospec::savina::fork_join;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_job final : public so_5::message_t {
    std::uint64_t index;
    explicit msg_job(std::uint64_t i) noexcept : index(i) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_done final : public so_5::message_t {
    std::uint64_t acc;
    std::uint64_t received;
    msg_done(std::uint64_t a, std::uint64_t r) noexcept : acc(a), received(r) {}
};

class worker_t final : public so_5::agent_t {
    const so_5::mbox_t  m_master;
    const std::uint64_t m_self;
    const std::uint64_t m_messages;
    const int           m_work;
    std::uint64_t       m_acc{0};
    std::uint64_t       m_received{0};

public:
    worker_t(context_t ctx, so_5::mbox_t master, std::uint64_t self, std::uint64_t messages,
             int work)
        : so_5::agent_t{std::move(ctx)}
        , m_master{std::move(master)}
        , m_self{self}
        , m_messages{messages}
        , m_work{work} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_job> j) {
            m_acc += job_value(m_self, j->index, m_work);
            if (++m_received == m_messages) so_5::send<msg_done>(m_master, m_acc, m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_master); }
};

class master_t final : public so_5::agent_t {
    const std::uint64_t       m_messages;
    qvo::Watch               &m_watch;
    Sink                     &m_sink;
    std::vector<so_5::mbox_t> m_workers;
    std::size_t               m_ready{0};
    std::size_t               m_done{0};

public:
    master_t(context_t ctx, std::uint64_t messages, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}, m_messages{messages}, m_watch{watch}, m_sink{sink} {}

    void set_workers(std::vector<so_5::mbox_t> workers) { m_workers = std::move(workers); }

    void so_define_agent() override {
        // The window opens once every worker has started: every work thread running and warm.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_workers.size()) return;
            m_watch.start();
            for (std::uint64_t i = 0; i < m_messages; ++i)
                for (const auto &w : m_workers) so_5::send<msg_job>(w, i);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done> d) {
            m_sink.checksum += d->acc;
            m_sink.messages += d->received + 1;
            if (++m_done != m_workers.size()) return;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto actors   = static_cast<std::size_t>(p.get("actors"));
    const auto messages = static_cast<std::uint64_t>(p.get("messages"));
    const auto work     = static_cast<int>(p.get("work"));
    const auto cores    = static_cast<int>(p.get("cores"));
    const bool spin     = p.get("wait") != 0;

    Sink sink;

    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            auto *master = coop.make_agent<master_t>(messages, std::ref(watch), std::ref(sink));
            std::vector<so_5::mbox_t> workers;
            workers.reserve(actors);
            for (std::size_t w = 0; w < actors; ++w)
                workers.push_back(coop.make_agent<worker_t>(master->so_direct_mbox(),
                                                            static_cast<std::uint64_t>(w),
                                                            messages, work)
                                      ->so_direct_mbox());
            master->set_workers(std::move(workers));
        });
    });

    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_fork_join_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join::params();
    spec.expected          = qvospec::savina::fork_join::expected;
    spec.expected_messages = qvospec::savina::fork_join::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join::work_units;
    spec.idiom_source      = "the ping-pong + counting adapters + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "agents on their DIRECT mboxes, one msg_job per job, thread_pool(cores) "
                             "with fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "the sixty workers are placed by the thread_pool: a free work thread takes the next "
        "agent with pending demands, the balancing qb's static placement does not do -- see "
        "benchmarks/savina/fork-join.md");

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_sobjectizer::body);
}
