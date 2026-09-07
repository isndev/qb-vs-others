// @benchmark     savina/bank-transaction
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the
//                teller is actor 0 on worker 0, account a is actor 1 + a, so account a is owned
//                by worker (1 + a) % cores -- the placement qb's cell fixes.
// @idiom-note    There is no request/reply here and nothing to allocate for one: a deposit is
//                one ring push to the destination's worker, the reply one push back, and the
//                "continuation" is a busy flag and a deque per account slot -- the state every
//                framework's primitive keeps for the caller, kept by hand. What the floor lacks,
//                and the frameworks pay for, is a MAILBOX per account and a request frame per
//                transfer: here two workers share one SPSC ring per direction, so the
//                1000-writer fan-in of acknowledgements into the teller and the per-transfer
//                frame are both engineered out. That is the point of the floor.

#include <qvospec/savina/bank-transaction.h>

#include "../baseline_support.h"

#include <deque>
#include <vector>

namespace savina_bank_transaction_baseline {

using namespace qvospec::savina::bank_transaction;

// kTransfer: a = dst << 32 | txn, b = amount.   kDeposit: a = src << 32 | txn, b = amount.
// kReply / kAck: a = txn.   kExit: none.   kReport: a = received << 32 | index, b = balance.
enum Tag : std::uint32_t {
    kTransfer = 1,
    kDeposit  = 2,
    kReply    = 3,
    kAck      = 4,
    kExit     = 5,
    kReport   = 6
};

struct Pending {
    std::uint32_t dst;
    std::uint64_t amount;
    std::uint64_t txn;
};

struct Account {
    std::uint64_t       balance{0};
    bool                busy{false};
    std::deque<Pending> held;
    std::uint64_t       received{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     accounts     = static_cast<std::uint32_t>(p.get("accounts"));
    const auto     transactions = static_cast<std::uint64_t>(p.get("transactions"));
    const auto     cores        = static_cast<unsigned>(p.get("cores"));
    const bool     spin         = p.get("wait") != 0;
    const unsigned W            = cores < 1 ? 1u : cores;

    const std::uint32_t teller   = 0;
    auto                actor_of = [](std::uint32_t a) { return 1 + a; };
    auto                index_of = [](std::uint32_t actor) { return actor - 1; };

    std::vector<Account> state(accounts);
    std::uint64_t        teller_received = 0;
    std::uint64_t        acked           = 0;
    std::uint32_t        reported        = 0;
    std::uint64_t        result          = 0;
    std::uint64_t        delivered       = 0;

    auto start = [&](auto &m, unsigned worker, std::uint32_t src, const Pending &t) {
        Account &a = state[src];
        a.busy     = true;
        a.balance -= t.amount;
        m.send(worker, qvobase::Msg{actor_of(t.dst), kDeposit,
                                    (std::uint64_t{src} << 32) | t.txn, t.amount});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kTransfer: {
            const std::uint32_t src = index_of(msg.dst);
            Account            &a   = state[src];
            ++a.received;
            const Pending t{static_cast<std::uint32_t>(msg.a >> 32), msg.b,
                            msg.a & 0xffffffffu};
            if (a.busy)
                a.held.push_back(t);
            else
                start(m, worker, src, t);
            break;
        }
        case kDeposit: {
            Account &a = state[index_of(msg.dst)];
            ++a.received;
            a.balance += msg.b;
            m.send(worker, qvobase::Msg{actor_of(static_cast<std::uint32_t>(msg.a >> 32)),
                                        kReply, msg.a & 0xffffffffu, 0});
            break;
        }
        case kReply: {
            const std::uint32_t src = index_of(msg.dst);
            Account            &a   = state[src];
            ++a.received;
            m.send(worker, qvobase::Msg{teller, kAck, msg.a, 0});
            if (a.held.empty()) {
                a.busy = false;
                break;
            }
            const Pending next = a.held.front();
            a.held.pop_front();
            start(m, worker, src, next);
            break;
        }
        case kAck:
            ++teller_received;
            result += qvo::mix(msg.a);
            if (++acked == transactions)
                for (std::uint32_t a = 0; a < accounts; ++a)
                    m.send(worker, qvobase::Msg{actor_of(a), kExit, 0, 0});
            break;
        case kExit: {
            const std::uint32_t index = index_of(msg.dst);
            Account            &a     = state[index];
            ++a.received;
            m.send(worker, qvobase::Msg{teller, kReport, (a.received << 32) | index, a.balance});
            break;
        }
        case kReport:
            ++teller_received;
            result += account_key(static_cast<std::uint32_t>(msg.a & 0xffffffffu), msg.b);
            delivered += msg.a >> 32;
            if (++reported == accounts) {
                delivered += teller_received;
                watch.stop();
                m.stop();
            }
            break;
        }
    });

    mesh.start();
    watch.start();
    for (std::uint64_t i = 0; i < transactions; ++i) {
        const Transfer t = transfer(i, accounts);
        mesh.send(0, qvobase::Msg{actor_of(t.src), kTransfer,
                                  (std::uint64_t{t.dst} << 32) | i, t.amount});
    }
    mesh.run();
    return qvo::Answer{result, delivered};
}

}  // namespace savina_bank_transaction_baseline

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
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; the teller is owned by thread 0 and account a by thread "
                             "(1 + a) % cores; a deposit is a push and its reply a push back, "
                             "the continuation a busy flag and a deque per slot; not an actor "
                             "framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Accounts are slots of a vector with no mailbox, and a transfer "
        "has no request frame: the continuation is a flag and a deque the handler reads, so "
        "the per-transfer allocation qb::ask and CAF's request().then() pay is engineered out",
        "the teller lives on thread 0 and account a on thread (1 + a) % cores -- the same "
        "placement qb's cell fixes -- so with cores=2 about half the deposits and half the "
        "transfers and acknowledgements cross a core",
        "the 50 000 transfers are pushed into the rings BEFORE worker 0 runs, from the caller's "
        "thread, so the teller's burst is the ring capacity the spec keeps N under, not a "
        "mailbox that grows",
        "cores=1 is one thread with the teller and the accounts in its own ring -- the floor "
        "for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable, which "
        "with a thousand accounts in flight is rare"};

    return qvo::run(argc, argv, std::move(spec), savina_bank_transaction_baseline::body);
}
