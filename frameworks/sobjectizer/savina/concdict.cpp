// @benchmark     savina/concdict
// @framework     sobjectizer 5.8.5.1
// @idiom-source  dev/sample/so_5/mutable_msg_agents/main.cpp -- SObjectizer's OWN sample of a
//                `mutable_msg` handed on with `so_5::send(next, std::move(cmd))`, no copy and no
//                allocation (dev/so_5/send_functions.hpp, "redirection of a message from existing
//                message hood", since 5.5.19); the bank-transaction adapter beside this file for the
//                agents on their DIRECT mboxes and the thread_pool binder (qvoso::make_pool_binder).
// @idiom-note    form=0, the reference's shape: a worker's request is ONE `mutable_msg<msg_op>`
//                that travels worker -> dictionary -> worker for the worker's whole run. The
//                dictionary does the lookup, writes the answer into the message and resends it to
//                the worker's direct mbox (the message names its worker by index); the worker folds
//                the answer, rewrites the message into its next request and resends it. Only the
//                first request is allocated. A mutable message may go to an MPSC mbox only, which a
//                direct mbox is. SObjectizer 5.8 has no asynchronous request/reply with a
//                continuation (request_value was removed in 5.6; so_5::extra::async_op is a separate
//                library), so form=1 is not applicable. Master, dictionary and workers on a
//                thread_pool of `cores` pinned work threads with fifo_t::individual, placed by the
//                dispatcher.

#include <qvospec/savina/concdict.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_concdict_sobjectizer {

using namespace qvospec::savina::concdict;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

// A request and, once the dictionary has written `value`, its answer -- one mutable message for
// both directions. `value` is the value a write stores on the way in, the value stored or read on
// the way back.
struct msg_op final : public so_5::message_t {
    std::uint32_t worker;
    std::uint32_t key;
    std::uint32_t value;
    bool          write;
    msg_op(std::uint32_t w, std::uint32_t k, std::uint32_t v, bool wr) noexcept
        : worker(w), key(k), value(v), write(wr) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_start final : public so_5::signal_t {};
struct msg_done final : public so_5::message_t {
    std::uint32_t worker;
    std::uint64_t fold;
    std::uint64_t received;
    msg_done(std::uint32_t w, std::uint64_t f, std::uint64_t r) noexcept : worker(w), fold(f), received(r) {}
};
struct msg_exit final : public so_5::signal_t {};
struct msg_report final : public so_5::message_t {
    std::uint64_t digest;
    std::uint64_t received;
    msg_report(std::uint64_t d, std::uint64_t r) noexcept : digest(d), received(r) {}
};

struct Field {
    so_5::mbox_t              master;
    so_5::mbox_t              dictionary;
    std::vector<so_5::mbox_t> workers;
};

class dictionary_t final : public so_5::agent_t {
    const Field  &m_field;
    Store        &m_store;
    std::uint64_t m_received{0};

public:
    dictionary_t(context_t ctx, const Field &field, Store &store)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_store{store} {}

    void so_define_agent() override {
        // Savina's Dictionary.process: the lookup or the put, answered to the worker that asked.
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_op> cmd) {
            ++m_received;
            cmd->value = cmd->write ? m_store.write(cmd->key, cmd->value) : m_store.read(cmd->key);
            const auto &to = m_field.workers[cmd->worker];
            so_5::send(to, std::move(cmd));
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++m_received;
            so_5::send<msg_report>(m_field.master, m_store.digest(), m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class worker_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    const Shape         m_shape;
    std::uint64_t       m_next{0};  // the index of the request in flight
    std::uint64_t       m_fold{0};
    std::uint64_t       m_received{0};

public:
    worker_t(context_t ctx, const Field &field, std::uint32_t index, Shape shape)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_index{index}
        , m_shape{shape} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_start>) {
            ++m_received;
            const Operation o = operation(m_shape, m_index, 0);
            so_5::send<so_5::mutable_msg<msg_op>>(m_field.dictionary, m_index, o.key, o.value, o.write);
        });
        // The answer to request `m_next`: folded, then the same message goes back as the next
        // request -- or, after the last, the worker reports.
        so_subscribe_self().event([this](so_5::mutable_mhood_t<msg_op> cmd) {
            ++m_received;
            if (m_next >= m_shape.messages) fail("a worker answered after its last request");
            m_fold += reply_key(m_index, m_next, cmd->value);
            if (++m_next == m_shape.messages) {
                so_5::send<msg_done>(m_field.master, m_index, m_fold, m_received);
                return;
            }
            const Operation o = operation(m_shape, m_index, m_next);
            cmd->key          = o.key;
            cmd->value        = o.value;
            cmd->write        = o.write;
            so_5::send(m_field.dictionary, std::move(cmd));
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.master); }
};

class master_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_workers;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    std::uint32_t       m_ready{0};
    std::uint32_t       m_done{0};
    std::uint64_t       m_received{0};

public:
    master_t(context_t ctx, const Field &field, std::uint32_t workers, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_workers{workers}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        // Every worker and the dictionary are up: open the window and start the workers.
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_workers + 1) return;
            m_watch.start();
            for (const auto &w : m_field.workers) so_5::send<msg_start>(w);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_done> m) {
            ++m_received;
            m_sink.checksum += done_key(m->worker, m->fold);
            m_sink.messages += m->received;
            if (++m_done != m_workers) return;
            m_watch.stop();  // every request answered and every worker done
            so_5::send<msg_exit>(m_field.dictionary);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_report> r) {
            ++m_received;
            m_sink.checksum += digest_key(r->digest);
            m_sink.messages += r->received + m_received;
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape shape = qvospec::savina::concdict::shape(p);
    const auto  cores = static_cast<int>(p.get("cores"));
    const bool  spin  = p.get("wait") != 0;
    if (asks(p))
        qvo::not_applicable("SObjectizer 5.8 has no asynchronous request/reply with a continuation "
                            "(request_value was removed in 5.6; so_5::extra::async_op is a separate "
                            "library), so form=1 has no counterpart here -- its form=0 cell answers "
                            "the sender with a plain message, which is all SObjectizer offers");

    Store store(shape.keys);  // before the environment, by this thread -- as in every adapter
    Sink  sink;
    Field field;
    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.master = coop.make_agent<master_t>(std::cref(field), shape.workers, std::ref(watch),
                                                     std::ref(sink))
                               ->so_direct_mbox();
            field.dictionary =
                coop.make_agent<dictionary_t>(std::cref(field), std::ref(store))->so_direct_mbox();
            field.workers.reserve(shape.workers);
            for (std::uint32_t w = 0; w < shape.workers; ++w)
                field.workers.push_back(
                    coop.make_agent<worker_t>(std::cref(field), w, shape)->so_direct_mbox());
        });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_concdict_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::concdict::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::concdict::expected;
    spec.expected_messages = qvospec::savina::concdict::expected_messages;
    spec.work_unit         = qvospec::savina::concdict::kWorkUnit;
    spec.work_units        = qvospec::savina::concdict::work_units;
    spec.idiom_source      = "dev/sample/so_5/mutable_msg_agents/main.cpp (a mutable_msg resent "
                             "with so_5::send(next, std::move(cmd))) + the bank-transaction adapter";
    spec.idiom_note        = "one mutable_msg<msg_op> per worker travels worker <-> dictionary on the "
                             "direct mboxes for the whole run, rewritten in place; no request/reply "
                             "primitive (form=1 not applicable); thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "form=0 resends ONE mutable message per worker for all its requests (SObjectizer's own "
        "mutable_msg redirection, its faster idiom: the documentation's lead -- so_5::send<msg>() "
        "of a new message per request and per answer -- allocates two messages per round trip); "
        "qb's reply() recycles one event the same way, CAF builds a message per request and per "
        "answer");
    spec.caveats.emplace_back(
        "SObjectizer 5.8 has no asynchronous request/reply with a continuation, so the form=1 "
        "documents of this benchmark are not applicable here, by the harness's third verdict");
    spec.caveats.emplace_back(
        "the master, the dictionary and the 20 workers are placed by the thread_pool; qb's cell "
        "pins the master and the dictionary on core 0 and worker w on core (1 + w) % cores -- see "
        "benchmarks/savina/concdict.md");
    spec.caveats.emplace_back(
        "the dictionary is the spec's Store (one std::unordered_map of `keys` entries, the same "
        "object code in every adapter), built before the environment by the thread that runs the "
        "repetition; its lookups are part of every framework's figure");

    return qvo::run(argc, argv, std::move(spec), savina_concdict_sobjectizer::body);
}
