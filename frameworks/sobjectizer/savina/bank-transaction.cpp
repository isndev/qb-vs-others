// @benchmark     savina/bank-transaction
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the chameneos adapter beside this file (agent_t subclasses on their DIRECT
//                mboxes, messages derived from so_5::message_t, the field's mboxes shared
//                before start) and dev/so_5/disp/thread_pool/pub.hpp via
//                qvoso::make_pool_binder.
// @idiom-note    SObjectizer has no asynchronous request/reply with a continuation:
//                `so_5::request_value` was removed in 5.6 and `so_5::extra::async_op` is a
//                separate library with a per-operation allocation. So the deposit is two plain
//                messages -- msg_deposit(amount, txn) to the destination's direct mbox and
//                msg_reply(txn) straight back to the source's -- and the "continuation" is the
//                account's own state: `busy` and the deque of held transfers, exactly the state
//                qb's and CAF's primitives keep for the caller. What SObjectizer pays here is
//                its dispatch, not a request frame; what it saves is the frame. Teller and
//                accounts on a thread_pool of `cores` pinned work threads with
//                fifo_t::individual, placed by the dispatcher.

#include <qvospec/savina/bank-transaction.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <deque>
#include <vector>

namespace savina_bank_transaction_sobjectizer {

using namespace qvospec::savina::bank_transaction;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct msg_transfer final : public so_5::message_t {
    std::uint32_t dst;
    std::uint64_t amount;
    std::uint64_t txn;
    msg_transfer(std::uint32_t d, std::uint64_t a, std::uint64_t t) noexcept
        : dst(d), amount(a), txn(t) {}
};
struct msg_deposit final : public so_5::message_t {
    std::uint32_t src;
    std::uint64_t amount;
    std::uint64_t txn;
    msg_deposit(std::uint32_t s, std::uint64_t a, std::uint64_t t) noexcept
        : src(s), amount(a), txn(t) {}
};
struct msg_reply final : public so_5::message_t {
    std::uint64_t txn;
    explicit msg_reply(std::uint64_t t) noexcept : txn(t) {}
};
struct msg_ack final : public so_5::message_t {
    std::uint64_t txn;
    explicit msg_ack(std::uint64_t t) noexcept : txn(t) {}
};
struct msg_ready final : public so_5::signal_t {};
struct msg_exit final : public so_5::signal_t {};
struct msg_report final : public so_5::message_t {
    std::uint32_t index;
    std::uint64_t balance;
    std::uint64_t received;
    msg_report(std::uint32_t i, std::uint64_t b, std::uint64_t r) noexcept
        : index(i), balance(b), received(r) {}
};

struct Field {
    std::vector<so_5::mbox_t> accounts;
    so_5::mbox_t              teller;
};

struct Pending {
    std::uint32_t dst;
    std::uint64_t amount;
    std::uint64_t txn;
};

class account_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint32_t m_index;
    std::uint64_t       m_balance{0};
    bool                m_busy{false};
    std::deque<Pending> m_held;
    std::uint64_t       m_received{0};

    void start(const Pending &p) {
        m_busy = true;
        m_balance -= p.amount;
        so_5::send<msg_deposit>(m_field.accounts[p.dst], m_index, p.amount, p.txn);
    }

public:
    account_t(context_t ctx, const Field &field, std::uint32_t index)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_index{index} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_transfer> m) {
            ++m_received;
            const Pending p{m->dst, m->amount, m->txn};
            if (m_busy)
                m_held.push_back(p);
            else
                start(p);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_deposit> m) {
            ++m_received;
            m_balance += m->amount;
            so_5::send<msg_reply>(m_field.accounts[m->src], m->txn);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_reply> m) {
            ++m_received;
            so_5::send<msg_ack>(m_field.teller, m->txn);
            if (m_held.empty()) {
                m_busy = false;
                return;
            }
            const Pending next = m_held.front();
            m_held.pop_front();
            start(next);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_exit>) {
            ++m_received;
            so_5::send<msg_report>(m_field.teller, m_index, m_balance, m_received);
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_field.teller); }
};

class teller_t final : public so_5::agent_t {
    const Field        &m_field;
    const std::uint64_t m_transactions;
    qvo::Watch         &m_watch;
    Sink               &m_sink;
    std::size_t         m_ready{0};
    std::uint64_t       m_acked{0};
    std::size_t         m_reported{0};
    std::uint64_t       m_received{0};

public:
    teller_t(context_t ctx, const Field &field, std::uint64_t transactions, qvo::Watch &watch,
             Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_field{field}
        , m_transactions{transactions}
        , m_watch{watch}
        , m_sink{sink} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_ready>) {
            if (++m_ready != m_field.accounts.size()) return;
            m_watch.start();
            const auto accounts = static_cast<std::uint32_t>(m_field.accounts.size());
            for (std::uint64_t i = 0; i < m_transactions; ++i) {
                const Transfer t = transfer(i, accounts);
                so_5::send<msg_transfer>(m_field.accounts[t.src], t.dst, t.amount, i);
            }
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_ack> m) {
            ++m_received;
            m_sink.checksum += qvo::mix(m->txn);
            if (++m_acked != m_transactions) return;
            for (const auto &a : m_field.accounts) so_5::send<msg_exit>(a);
        });
        so_subscribe_self().event([this](so_5::mhood_t<msg_report> r) {
            ++m_received;
            m_sink.checksum += account_key(r->index, r->balance);
            m_sink.messages += r->received;
            if (++m_reported != m_field.accounts.size()) return;
            m_sink.messages += m_received;
            m_watch.stop();
            so_environment().stop();
        });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto accounts     = static_cast<std::uint32_t>(p.get("accounts"));
    const auto transactions = static_cast<std::uint64_t>(p.get("transactions"));
    const auto cores        = static_cast<int>(p.get("cores"));
    const bool spin         = p.get("wait") != 0;

    Sink  sink;
    Field field;
    so_5::launch([&](so_5::environment_t &env) {
        env.introduce_coop(qvoso::make_pool_binder(env, cores, spin), [&](so_5::coop_t &coop) {
            field.teller = coop.make_agent<teller_t>(std::cref(field), transactions,
                                                     std::ref(watch), std::ref(sink))
                               ->so_direct_mbox();
            field.accounts.reserve(accounts);
            for (std::uint32_t a = 0; a < accounts; ++a)
                field.accounts.push_back(
                    coop.make_agent<account_t>(std::cref(field), a)->so_direct_mbox());
        });
    });
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_bank_transaction_sobjectizer

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bank_transaction::params();
    spec.expected          = qvospec::savina::bank_transaction::expected;
    spec.expected_messages = qvospec::savina::bank_transaction::expected_messages;
    spec.work_unit         = qvospec::savina::bank_transaction::kWorkUnit;
    spec.work_units        = qvospec::savina::bank_transaction::work_units;
    spec.idiom_source      = "the chameneos adapter + dev/so_5/disp/thread_pool/pub.hpp";
    spec.idiom_note        = "no request/reply primitive: msg_deposit to the destination's direct "
                             "mbox and msg_reply back to the source's, the continuation is the "
                             "account's own busy flag and deque; thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer 5.8 has no asynchronous request/reply with a continuation (request_value "
        "was removed in 5.6, so_5::extra::async_op is a separate library), so the deposit is "
        "two plain messages and the caller's continuation is hand-written state -- this cell "
        "measures the dispatch alone and pays no request frame, unlike qb::ask and CAF's "
        "request().then()");
    spec.caveats.emplace_back(
        "the teller and the 1000 accounts are placed by the thread_pool; qb's cell pins the "
        "teller on core 0 and account a on core (1 + a) % cores -- see "
        "benchmarks/savina/bank-transaction.md");

    return qvo::run(argc, argv, std::move(spec), savina_bank_transaction_sobjectizer::body);
}
