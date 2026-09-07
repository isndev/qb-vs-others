// @benchmark     savina/bank-transaction
// @framework     qb
// @idiom-source  qb/src/qb/core/patterns/request.h (`qb::ask`, `AskEvent`, `resolve_ask`),
//                qb/tests/core/benchmark/messaging/ask-roundtrip.cpp (the spawn + ask loop) and
//                qb/llm/qb.llm.md section 4 for the coroutine capture rule.
// @idiom-note    The deposit is qb's OWN request/reply: the source account `co_await`s
//                `qb::ask<Deposit>(ctx, dst, 0, amount, txn)` from a coroutine it spawned, the
//                destination `reply()`s the same event, and the source's `on(Deposit&)` routes
//                the answer with `resolve_ask` before it treats anything as a fresh deposit.
//                That is the EMPLACE form of `ask` (3.2: the request is constructed in the pipe
//                slot, nothing is copied); a qb without it — v3.1.0 — gets the by-value
//                `qb::ask(ctx, dst, Deposit{...}, 0)` from the same source, chosen at compile
//                time, so shipped and candidate each run the ask their version recommends.
//                One coroutine per account at a time: it drains the account's queue of held
//                transfers and returns when the queue is empty, so an account with transfers
//                arriving back to back keeps one frame alive and an idle account holds none.
//                The frame captures a shared_ptr to the account's state, never `this`: after a
//                suspension the actor may already be gone (qb.llm.md section 4). The teller
//                lives on VirtualCore 0 with half the accounts; account a is on core
//                (1 + a) % cores, so with cores=2 about half the deposits and half the
//                transfers cross a core.

#include <qvospec/savina/bank-transaction.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/core/patterns/request.h>
#include <qb/main.h>

#include <concepts>
#include <deque>
#include <memory>
#include <vector>

namespace savina_bank_transaction_qb {

using namespace qvospec::savina::bank_transaction;

// teller -> source account: move `amount` to `dst`, for transfer `txn`.
struct TransferEvent : qb::Event {
    std::uint32_t dst;
    std::uint64_t amount;
    std::uint64_t txn;
    TransferEvent(std::uint32_t d, std::uint64_t a, std::uint64_t t) noexcept
        : dst(d), amount(a), txn(t) {}
};
// source -> destination, and back: the ask. `txn` rides along so the continuation needs no
// state of its own to acknowledge.
struct Deposit : qb::AskEvent {
    std::uint64_t amount;
    std::uint64_t txn;
    Deposit(std::uint64_t a, std::uint64_t t) noexcept : amount(a), txn(t) {}
};
// source -> teller: transfer `txn` is complete.
struct Ack : qb::Event {
    std::uint64_t txn;
    explicit Ack(std::uint64_t t) noexcept : txn(t) {}
};
struct Ready : qb::Event {};
struct Exit : qb::Event {};
struct Report : qb::Event {
    std::uint32_t index;
    std::uint64_t balance;
    std::uint64_t received;
    Report(std::uint32_t i, std::uint64_t b, std::uint64_t r) noexcept
        : index(i), balance(b), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct Field {
    std::vector<qb::ActorId> accounts;
    qb::ActorId              teller;
};

struct Pending {
    std::uint32_t dst;
    std::uint64_t amount;
    std::uint64_t txn;
};

// What the coroutine and the actor share. The coroutine owns a reference through a shared_ptr
// and never touches the actor; the actor is the only one to read `balance` after the frame is
// gone, at exit, when no transfer can be in flight.
struct State {
    std::uint64_t       balance{0};
    bool                busy{false};
    std::deque<Pending> held;
};

// The emplace `ask` (qb 3.2, request.h): `qb::ask<E>(ctx, target, timeout, args...)` builds the
// event in the outgoing pipe slot. Detected rather than version-gated, like HasIdleSpin in
// qb_support.h: the branch the qb at hand cannot compile is discarded, and each version runs
// the ask its own request.h recommends.
template <typename Ctx>
concept HasEmplaceAsk = requires(const Ctx &ctx, qb::ActorId id, std::uint64_t v) {
    { qb::ask<Deposit>(ctx, id, qb::duration::zero(), v, v) } -> std::same_as<qb::io::async::task<Deposit>>;
};

template <typename Ctx>
qb::io::async::task<Deposit> deposit(const Ctx &ctx, qb::ActorId dst, std::uint64_t amount, std::uint64_t txn) {
    if constexpr (HasEmplaceAsk<Ctx>)
        return qb::ask<Deposit>(ctx, dst, qb::duration::zero(), amount, txn);
    else
        return qb::ask(ctx, dst, Deposit{amount, txn}, qb::duration::zero());
}

class Account final : public qb::Actor {
    const Field           &_field;
    const std::uint32_t    _index;
    std::shared_ptr<State> _st{std::make_shared<State>()};
    std::uint64_t          _received{0};

    // Start the transfer now: debit, then hand the deposit to a coroutine that asks the
    // destination and acknowledges when the answer lands, and keeps going while transfers were
    // held meanwhile. Everything the frame needs is captured by value before its first
    // suspension.
    void start(Pending first) {
        _st->busy = true;
        _st->balance -= first.amount;
        spawn([st = _st, teller = _field.teller, accounts = &_field.accounts,
               first](qb::ScopedCoroContext ctx) -> qb::io::async::task<void> {
            Pending cur = first;
            for (;;) {
                co_await deposit(ctx, (*accounts)[cur.dst], cur.amount, cur.txn);
                ctx.push_to<Ack>(teller, cur.txn);
                if (st->held.empty()) {
                    st->busy = false;
                    co_return;
                }
                cur = st->held.front();
                st->held.pop_front();
                st->balance -= cur.amount;
            }
        });
    }

public:
    Account(const Field &field, std::uint32_t index) noexcept : _field(field), _index(index) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<TransferEvent>(*this);
        registerEvent<Deposit>(*this);
        registerEvent<Exit>(*this);
        push<Ready>(_field.teller);
        co_return true;
    }

    void on(TransferEvent const &event) {
        ++_received;
        const Pending p{event.dst, event.amount, event.txn};
        if (_st->busy)
            _st->held.push_back(p);
        else
            start(p);
    }

    void on(Deposit &event) {
        ++_received;
        if (resolve_ask(event)) return;  // the destination's answer to one of our own asks
        _st->balance += event.amount;
        reply(event);
    }

    void on(Exit const &) {
        ++_received;
        push<Report>(_field.teller, _index, _st->balance, _received);
    }
};

class Teller final : public qb::Actor {
    const Field        &_field;
    const std::uint64_t _transactions;
    qvo::Watch         &_watch;
    Sink               &_sink;
    std::size_t         _ready{0};
    std::uint64_t       _acked{0};
    std::size_t         _reported{0};
    std::uint64_t       _received{0};

public:
    Teller(const Field &field, std::uint64_t transactions, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _transactions(transactions), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Ack>(*this);
        registerEvent<Report>(*this);
        co_return true;
    }

    void on(Ready const &) {
        if (++_ready != _field.accounts.size()) return;
        _watch.start();
        const auto accounts = static_cast<std::uint32_t>(_field.accounts.size());
        for (std::uint64_t i = 0; i < _transactions; ++i) {
            const Transfer t = transfer(i, accounts);
            push<TransferEvent>(_field.accounts[t.src], t.dst, t.amount, i);
        }
    }

    void on(Ack const &event) {
        ++_received;
        _sink.checksum += qvo::mix(event.txn);
        if (++_acked != _transactions) return;
        for (const auto &a : _field.accounts) push<Exit>(a);
    }

    void on(Report const &event) {
        ++_received;
        _sink.checksum += account_key(event.index, event.balance);
        _sink.messages += event.received;
        if (++_reported != _field.accounts.size()) return;
        _sink.messages += _received;
        _watch.stop();
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto accounts     = static_cast<std::uint32_t>(p.get("accounts"));
    const auto transactions = static_cast<std::uint64_t>(p.get("transactions"));
    const auto cores        = static_cast<int>(p.get("cores"));
    const bool spin         = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.teller = engine.addActor<Teller>(0, std::cref(field), transactions, std::ref(watch),
                                               std::ref(sink));
        field.accounts.reserve(accounts);
        for (std::uint32_t a = 0; a < accounts; ++a) {
            // The teller is on core 0; account a on core (1 + a) % cores, so the teller shares
            // its core with half the accounts and the other half is one pipe away.
            const int core = (1 + static_cast<int>(a)) % ncores;
            field.accounts.push_back(engine.addActor<Account>(static_cast<qb::CoreId>(core),
                                                              std::cref(field), a));
        }

        engine.start();
        engine.join();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_bank_transaction_qb

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
    spec.idiom_source      = "qb/src/qb/core/patterns/request.h (qb::ask) + "
                             "qb/tests/core/benchmark/messaging/ask-roundtrip.cpp";
    spec.idiom_note        = "deposit = co_await qb::ask() from a coroutine the source account "
                             "spawns, one live frame per account at most, held transfers in a "
                             "deque the frame drains; teller on VirtualCore 0, account a on "
                             "core (1 + a) % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb::ask is a coroutine: each transfer that starts while the account is idle allocates a "
        "coroutine frame and registers a pending-ask slot on the core, and each reply is routed "
        "by correlation id and resumed through the scheduler -- that is the primitive under "
        "test, and it is the only shape in this suite where qb allocates per work unit");
    spec.caveats.emplace_back(
        "placement is fixed before start: the teller on VirtualCore 0 and account a on core "
        "(1 + a) % cores, so with cores=2 about half the deposits and half the transfers and "
        "acknowledgements cross a core and nothing rebalances it -- the pools place freely");

    return qvo::run(argc, argv, std::move(spec), savina_bank_transaction_qb::body);
}
