// savina/philosophers — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Dining Philosophers (Imam & Sarkar, AGERE 2014), in the "concurrency" group,
// `edu.rice.habanero.benchmarks.philosopher` -- the actor variants
// (PhilosopherAkkaActorBenchmark and its siblings), NOT the Habanero selector ones. Deviations are
// recorded in benchmarks/savina/philosophers.md.

#ifndef QVOSPEC_SAVINA_PHILOSOPHERS_H
#define QVOSPEC_SAVINA_PHILOSOPHERS_H

#include <qvo/harness.h>

#include <cstdint>

namespace qvospec::savina::philosophers {

inline constexpr const char *kId = "savina/philosophers";

// The shape: `philosophers` philosophers around a table of as many forks, and ONE arbitrator that
// owns every fork. Philosopher i needs forks i and (i + 1) % philosophers. Its cycle, exactly as
// Savina's actor variants write it:
//
//     Start   -> it sends Hungry(i) to the arbitrator.
//     Denied  -> it sends Hungry(i) again, at once (a busy retry -- Savina counts these).
//     Eat     -> it has eaten one meal: it sends Done(i) to the arbitrator, then Start to ITSELF,
//                or, after its `rounds`-th meal, Exit(i) to the arbitrator and ends.
//
// The arbitrator answers Hungry(i) with Eat when both forks are free (and marks them taken) and
// with Denied otherwise; Done(i) frees the two forks; the run ends at the `philosophers`-th Exit.
// It is a single mailbox written by every philosopher and answering every request -- the
// coordination hot spot chameneos has -- but with a REFUSAL path: a request that loses the race
// for a fork is answered, refused and re-sent, so the benchmark is what a framework costs when
// most of its traffic is contention that comes to nothing.
//
// HOW MANY requests are refused is the scheduler's decision and differs run to run (Savina
// reports it as "Num retries", measured, never asserted). It is not in the checksum and not in
// the asserted message count: the asserted value is built from the messages the protocol FIXES
// -- every Start, every grant, every meal, every Done and every Exit -- each identified by its
// philosopher and its round, so the value does not depend on who was refused how often (see
// expected). The refused pairs are still delivered and timed by every framework alike.
//
// Termination does not depend on fairness luck: every philosopher holds exactly one message of
// the cycle at any time, a fork is taken only by a granted philosopher whose Done is on its way,
// so whenever no Done is pending every fork is free and the next Hungry is granted; and each
// philosopher eats exactly `rounds` meals, so a philosopher refused for as long as its neighbours
// eat is granted at the latest once they have finished.
//
// `philosophers` -- Savina's N, default 20; no deviation.
// `rounds`       -- Savina's M (meals per philosopher), default 10 000; no deviation.
// `cores`        -- 1: the arbitrator and every philosopher on one thread; 2: the arbitrator on
//                   one pinned thread and the philosophers on the other for the frameworks that
//                   place, so every request and every answer crosses a core.
// `wait`         -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
// Savina's third parameter, C (channels, default 1), is read only by its Habanero SELECTOR
// variants; the actor variants this repository follows ignore it, and so does this spec.
inline std::map<std::string, long long> params() {
    return {{"philosophers", 20}, {"rounds", 10000}, {"cores", 2}, {"wait", 1}};
}

// The five messages the protocol fixes, each folded once, by its RECEIVER, as
// term(kind, philosopher, round) where `round` is the receiver's own 1-based count of that kind
// of message for that philosopher:
//
//     kStart  the philosopher, its r-th Start  (r = 1 .. rounds; the first from the arbitrator,
//             the others from itself)
//     kGrant  the arbitrator, its r-th Hungry(i) that it GRANTED (a refused Hungry folds nothing)
//     kEat    the philosopher, its r-th Eat
//     kDone   the arbitrator, its r-th Done(i)
//     kExit   the arbitrator, Exit(i), with `round` = the number of Done(i) it has received then
//
// A philosopher carries the sum of its own folds (and its own message count) in its Exit; the
// arbitrator adds them to its own. So a grant answered twice, a meal delivered twice or lost, a
// Done lost or duplicated, an Exit that overtook its last Done, or a philosopher that stopped
// early or ate once too often changes the total.
enum Kind : std::uint64_t { kStart = 1, kGrant = 2, kEat = 3, kDone = 4, kExit = 5 };

inline std::uint64_t term(Kind kind, std::uint64_t philosopher, std::uint64_t round) noexcept {
    return qvo::mix(qvo::mix((static_cast<std::uint64_t>(kind) << 56) | philosopher) + round);
}

// What the arbitrator folds, once, if it ever frees a fork that the philosopher sending Done did
// not hold: two philosophers ate with a shared fork, which only a misdelivered or duplicated
// message can cause. Never reached in a verified run; it exists so that such a run cannot verify.
inline constexpr std::uint64_t kForkViolation = 0x5afe'f0e5'dead'beefULL;

inline std::uint64_t expected(const qvo::Params &p) {
    const auto    n   = static_cast<std::uint64_t>(p.get("philosophers"));
    const auto    m   = static_cast<std::uint64_t>(p.get("rounds"));
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        for (std::uint64_t r = 1; r <= m; ++r)
            acc += term(kStart, i, r) + term(kGrant, i, r) + term(kEat, i, r) + term(kDone, i, r);
        acc += term(kExit, i, m);
    }
    return acc;
}

// The report divides by this: one meal -- a granted request, the meal, the fork release and the
// next Start. The refused requests a meal needed are inside it, and that is the point.
inline constexpr const char *kWorkUnit = "meal";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("philosophers")) *
           static_cast<std::uint64_t>(p.get("rounds"));
}

// The messages the protocol fixes, counted at the receivers: n * m Starts, n * m granted Hungry,
// n * m Eats, n * m Dones and n Exits. The refused pairs (a Denied and the Hungry it triggers, or
// the refused Hungry and its Denied) are NOT counted -- see above. A philosopher counts its Starts
// and Eats and carries the count in its Exit; the arbitrator counts grants, Dones and Exits.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const auto n = static_cast<std::uint64_t>(p.get("philosophers"));
    const auto m = static_cast<std::uint64_t>(p.get("rounds"));
    return 4 * n * m + n;
}

}  // namespace qvospec::savina::philosophers

#endif  // QVOSPEC_SAVINA_PHILOSOPHERS_H
