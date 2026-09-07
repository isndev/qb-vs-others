// savina/bank-transaction — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Bank Transaction / "banking" (Imam & Sarkar, AGERE 2014), the last of the
// "concurrency" group. Deviations are recorded in benchmarks/savina/bank-transaction.md.

#ifndef QVOSPEC_SAVINA_BANK_TRANSACTION_H
#define QVOSPEC_SAVINA_BANK_TRANSACTION_H

#include <qvo/harness.h>

#include <cstdint>
#include <vector>

namespace qvospec::savina::bank_transaction {

inline constexpr const char *kId = "savina/bank-transaction";

// The shape: ONE teller and `accounts` accounts. The teller sends `transactions` transfers, all
// at once, each to its SOURCE account: "move `amount` to `dst`". The source debits itself, asks
// the destination to deposit the amount and WAITS for that account's reply before it
// acknowledges the transfer to the teller; while it waits, further transfers addressed to it are
// held back and started one at a time, in order, as each reply arrives. A deposit is always
// served. So every transfer is a REQUEST/REPLY between two accounts nested inside a
// request/reply between the teller and one of them -- the shape that measures what a
// framework's request-with-continuation costs (qb::ask, CAF request().then(), a hand-written
// pending state where the framework has no primitive), on top of a thousand-writer fan-in of
// acknowledgements into the teller. No other shape in this suite waits for an answer.
//
// The set of transfers is a deterministic function of the index (transfer()), so an
// implementation cannot pick its own; the final balance of every account is fixed by that set
// alone, whatever the order the transfers complete in -- which is the scheduler's business and
// differs run to run. The checksum is built from those balances and from the acknowledgements
// (see expected).
//
// `accounts`     -- Savina's own default, 1000; no deviation.
// `transactions` -- Savina's own default, 50 000; no deviation. Four messages each inside the
//                   window (transfer, deposit, reply, acknowledgement): 200 000 messages a
//                   repetition, plus 2 000 for the exit.
// `cores`        -- 1: everything on one thread; 2: the teller on core 0 and account a on core
//                   (1 + a) % cores for the frameworks that place, so about half of the transfers
//                   and half of the deposits cross a core, and the teller shares its core with
//                   half the accounts: the balanced deal a work-stealing pool gives itself.
// `wait`         -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"accounts", 1000}, {"transactions", 50000}, {"cores", 2}, {"wait", 1}};
}

// Transfer i: Savina's own draw, made deterministic. The source is drawn from the first 80 % of
// the accounts and the destination is STRICTLY ABOVE it (Savina: `loopId == 0 -> 1`), which is
// what keeps a chain of waiting accounts from ever closing on itself. Savina draws a double
// amount in [0, 1000); this draws an integer in [1, 1000] so that a balance is exact arithmetic
// and a checksum over it is meaningful.
struct Transfer {
    std::uint32_t src;
    std::uint32_t dst;
    std::uint64_t amount;
};

inline Transfer transfer(std::uint64_t i, std::uint32_t accounts) noexcept {
    const std::uint64_t h    = qvo::mix(i + 1);
    const std::uint64_t h2   = qvo::mix(h);
    const auto          src  = static_cast<std::uint32_t>(h % ((accounts / 10) * 8));
    std::uint32_t       loop = static_cast<std::uint32_t>(h2 % (accounts - src));
    if (loop == 0) loop = 1;
    return Transfer{src, src + loop, 1 + (h2 >> 32) % 1000};
}

// Balances start at zero and wrap: a source account goes "negative" as an unsigned and that is
// fine, the final value is still fixed by the transfer set alone.
inline std::vector<std::uint64_t> final_balances(const qvo::Params &p) {
    const auto                 a = static_cast<std::uint32_t>(p.get("accounts"));
    const auto                 n = static_cast<std::uint64_t>(p.get("transactions"));
    std::vector<std::uint64_t> balance(a, 0);
    for (std::uint64_t i = 0; i < n; ++i) {
        const Transfer t = transfer(i, a);
        balance[t.src] -= t.amount;
        balance[t.dst] += t.amount;
    }
    return balance;
}

// What an account reports at exit, and what the teller folds in for it.
inline std::uint64_t account_key(std::uint32_t account, std::uint64_t balance) noexcept {
    return qvo::mix(balance + qvo::mix(account));
}

// The teller folds mix(i) for every acknowledgement it receives -- an acknowledgement carries
// the index of the transfer it completes -- and account_key() for every balance reported at
// exit. A transfer acknowledged twice, never acknowledged, or whose deposit landed on the wrong
// account, or an account that never reported, changes the total; the order in which transfers
// complete does not.
inline std::uint64_t expected(const qvo::Params &p) {
    const auto    n   = static_cast<std::uint64_t>(p.get("transactions"));
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < n; ++i) acc += qvo::mix(i);
    const auto balances = final_balances(p);
    for (std::uint32_t a = 0; a < balances.size(); ++a) acc += account_key(a, balances[a]);
    return acc;
}

// The report divides by this: one transfer -- a request from the teller, a request to the
// destination, its reply, and the acknowledgement.
inline constexpr const char *kWorkUnit = "transfer";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("transactions"));
}

// Inside the window: n transfers received by their source, n deposits received by their
// destination, n replies received by the source, n acknowledgements received by the teller;
// then a exits received by the accounts and a balance reports received by the teller. The
// readiness handshake that opens the window is outside it and not counted. Counted at the
// receivers: an account counts its transfers, deposits, replies and exit and carries the count
// in its report; the teller counts acknowledgements and reports.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const auto n = static_cast<std::uint64_t>(p.get("transactions"));
    const auto a = static_cast<std::uint64_t>(p.get("accounts"));
    return 4 * n + 2 * a;
}

}  // namespace qvospec::savina::bank_transaction

#endif  // QVOSPEC_SAVINA_BANK_TRANSACTION_H
