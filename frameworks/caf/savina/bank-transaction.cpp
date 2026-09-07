// @benchmark     savina/bank-transaction
// @framework     caf 1.1.0
// @idiom-source  the chameneos adapter beside this file (function-based behaviors,
//                stateful_actor, handles exchanged before the window, built-in atoms) and
//                caf/event_based_mail.hpp + caf/event_based_response_handle.hpp for
//                `mail(...).request(dst, infinite).then(...)`.
// @idiom-note    The deposit is CAF's OWN request/reply: the source account sends
//                `(put_atom, amount, txn)` with `.request(dst, caf::infinite).then(...)`, the
//                destination's handler RETURNS the value and CAF routes it back as the response
//                to that request id; the `.then` continuation acknowledges the teller and starts
//                the next held transfer. `then`, not `await`: the account keeps serving deposits
//                while a request is in flight, as the spec requires. A transfer is
//                `(add_atom, dst, amount, txn)`, the acknowledgement `(ok_atom, txn)`, the exit
//                `close_atom`, the report `(ok_atom, index, balance, messages)`. Every account
//                and the teller are placed by the work-stealing pool.

#include <qvospec/savina/bank-transaction.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <deque>
#include <utility>
#include <vector>

namespace savina_bank_transaction_caf {

using namespace qvospec::savina::bank_transaction;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
};

struct Pending {
    std::uint32_t dst;
    std::uint64_t amount;
    std::uint64_t txn;
};

struct account_state {
    caf::actor              teller;
    std::vector<caf::actor> accounts;
    std::uint32_t           index{0};
    std::uint64_t           balance{0};
    bool                    busy{false};
    std::deque<Pending>     held;
    std::uint64_t           received{0};
};

struct teller_state {
    std::vector<caf::actor> accounts;
    std::uint64_t           transactions{0};
    std::size_t             wired{0};
    std::uint64_t           acked{0};
    std::size_t             reported{0};
    std::uint64_t           received{0};
    qvo::Watch             *watch{nullptr};
    Sink                   *sink{nullptr};
};

caf::behavior account_fun(caf::stateful_actor<account_state> *self, std::uint32_t index) {
    self->state().index = index;

    // Debit and ask the destination; on its answer acknowledge and, if a transfer was held
    // meanwhile, start it from the continuation.
    auto start = [self](auto &&start_ref, Pending p) -> void {
        auto &s  = self->state();
        s.busy   = true;
        s.balance -= p.amount;
        self->mail(caf::put_atom_v, p.amount, p.txn)
            .request(s.accounts[p.dst], caf::infinite)
            .then([self, start_ref](std::uint64_t txn) {
                auto &st = self->state();
                ++st.received;  // the response message: received by this account like any other
                self->mail(caf::ok_atom_v, txn).send(st.teller);
                if (st.held.empty()) {
                    st.busy = false;
                    return;
                }
                const Pending next = st.held.front();
                st.held.pop_front();
                start_ref(start_ref, next);
            });
    };

    return {
        // Wiring handshake, once, outside the window.
        [self](caf::put_atom, caf::actor teller, std::vector<caf::actor> accounts) {
            auto &s    = self->state();
            s.teller   = std::move(teller);
            s.accounts = std::move(accounts);
            self->mail(caf::ok_atom_v).send(s.teller);
        },
        [self, start](caf::add_atom, std::uint32_t dst, std::uint64_t amount,
                      std::uint64_t txn) {
            auto &s = self->state();
            ++s.received;
            const Pending p{dst, amount, txn};
            if (s.busy)
                s.held.push_back(p);
            else
                start(start, p);
        },
        [self](caf::put_atom, std::uint64_t amount, std::uint64_t txn) -> std::uint64_t {
            auto &s = self->state();
            ++s.received;
            s.balance += amount;
            return txn;
        },
        [self](caf::close_atom) {
            auto &s = self->state();
            ++s.received;
            self->mail(caf::ok_atom_v, s.index, s.balance, s.received).send(s.teller);
            self->quit();
        },
    };
}

caf::behavior teller_fun(caf::stateful_actor<teller_state> *self, std::vector<caf::actor> accounts,
                         std::uint64_t transactions, qvo::Watch *watch, Sink *sink) {
    auto &st        = self->state();
    st.accounts     = std::move(accounts);
    st.transactions = transactions;
    st.watch        = watch;
    st.sink         = sink;

    const auto me = caf::actor_cast<caf::actor>(self);
    for (auto &a : st.accounts) self->mail(caf::put_atom_v, me, st.accounts).send(a);

    return {
        [self](caf::ok_atom) {
            auto &s = self->state();
            if (++s.wired != s.accounts.size()) return;
            s.watch->start();
            const auto accounts = static_cast<std::uint32_t>(s.accounts.size());
            for (std::uint64_t i = 0; i < s.transactions; ++i) {
                const Transfer t = transfer(i, accounts);
                self->mail(caf::add_atom_v, t.dst, t.amount, i).send(s.accounts[t.src]);
            }
        },
        [self](caf::ok_atom, std::uint64_t txn) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += qvo::mix(txn);
            if (++s.acked != s.transactions) return;
            for (auto &a : s.accounts) self->mail(caf::close_atom_v).send(a);
        },
        [self](caf::ok_atom, std::uint32_t index, std::uint64_t balance, std::uint64_t received) {
            auto &s = self->state();
            ++s.received;
            s.sink->checksum += account_key(index, balance);
            s.sink->messages += received;
            if (++s.reported != s.accounts.size()) return;
            s.sink->messages += s.received;
            s.watch->stop();
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto accounts     = static_cast<std::uint32_t>(p.get("accounts"));
    const auto transactions = static_cast<std::uint64_t>(p.get("transactions"));
    const auto cores        = static_cast<std::size_t>(p.get("cores"));
    const bool spin         = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        std::vector<caf::actor> field;
        field.reserve(accounts);
        for (std::uint32_t a = 0; a < accounts; ++a)
            field.push_back(sys.spawn<qvocaf::kSpawnOptions>(account_fun, a));
        sys.spawn<qvocaf::kSpawnOptions>(teller_fun, field, transactions, &watch, &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    return qvo::Answer{sink.checksum, sink.messages};
}

}  // namespace savina_bank_transaction_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bank_transaction::params();
    spec.expected          = qvospec::savina::bank_transaction::expected;
    spec.expected_messages = qvospec::savina::bank_transaction::expected_messages;
    spec.work_unit         = qvospec::savina::bank_transaction::kWorkUnit;
    spec.work_units        = qvospec::savina::bank_transaction::work_units;
    spec.idiom_source      = "caf/event_based_mail.hpp request().then() + the chameneos adapter";
    spec.idiom_note        = "deposit = mail(put_atom, amount, txn).request(dst, infinite).then(); "
                             "the responder returns txn; held transfers in a deque the "
                             "continuation drains; placement left to the pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "request().then() is CAF's multiplexed request: the continuation is a heap-allocated "
        "behavior keyed by the response id and the reply is a typed response message, so each "
        "transfer that starts allocates the continuation and the account keeps serving its "
        "mailbox meanwhile -- the primitive under test, on the same terms as qb::ask");
    spec.caveats.emplace_back(
        "the teller and the 1000 accounts are placed by the work-stealing pool; qb's cell pins "
        "the teller on core 0 and account a on core (1 + a) % cores -- see "
        "benchmarks/savina/bank-transaction.md");

    return qvo::run(argc, argv, std::move(spec), savina_bank_transaction_caf::body);
}
