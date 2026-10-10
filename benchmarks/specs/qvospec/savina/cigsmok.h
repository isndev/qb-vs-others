// savina/cigsmok — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Cigarette Smokers (Imam & Sarkar, AGERE 2014), one of the "concurrency"
// benchmarks -- CigaretteSmokerConfig.java and CigaretteSmokerAkkaActorBenchmark.scala.
// Deviations are recorded in benchmarks/savina/cigsmok.md.

#ifndef QVOSPEC_SAVINA_CIGSMOK_H
#define QVOSPEC_SAVINA_CIGSMOK_H

#include <qvo/harness.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace qvospec::savina::cigsmok {

inline constexpr const char *kId = "savina/cigsmok";

// The shape, Savina's own: ONE arbiter and `smokers` smokers. The arbiter puts ingredients on the
// table by choosing a smoker and telling it `StartSmoking` with a busy-wait period; the smoker
// tells the arbiter `StartedSmoking` FIRST -- the ingredients are off the table -- and only then
// smokes (busy-works the period). On `StartedSmoking` the arbiter counts a round and chooses the
// next smoker, so exactly ONE round is outstanding at any time, while the smokes of earlier rounds
// may still be running on other threads: the arbiter's turn and the smoking overlap, and how much
// they overlap is what the runtime decides. After `rounds` rounds the arbiter tells every smoker
// `Exit`; here each smoker answers with a `Report` (its share of the checksum), where Savina's
// actor exits and its runtime detects the termination.
//
// It is the arbiter shape of the suite: one actor decides, on every round, which ONE of many
// receives the next message -- a selective unicast, never a broadcast -- and the per-round cost is
// one decision, one request and one acknowledgement, with real work running behind it.
//
// `rounds`  -- rounds played. 50 000 where Savina's default is 1 000 -- a DEVIATION (cigsmok.md,
//              "Deviation from Savina's rounds, and why"): Savina's count is a window of 520 076
//              busy-work iterations in which one round in five is a smoker's first delivery. Pass
//              `--param rounds=1000` to reproduce Savina's count.
// `smokers` -- smokers, each an actor created with the arbiter before the window. Savina's own
//              default, 200; no deviation.
// `smoke`   -- the busy-wait bound: a smoke is `uniform[0, smoke) + 10` iterations. Savina
//              hard-codes 1 000 (`random.nextInt(1000) + 10`); declared here, at that value, so
//              the smoke can be shrunk: `smoke=1` gives every round a fixed 10-iteration smoke,
//              the least the distribution allows -- not the coordination alone.
// `cores`   -- 1: everything on one thread; 2: for the frameworks that place, the arbiter on
//              core 0 and smoker j on core (j + 1) % cores -- half of the smokes on the arbiter's
//              core, half beside it, so a smoke on the far core overlaps the arbiter's next
//              decision.
// `wait`    -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"rounds", 50000}, {"smokers", 200}, {"smoke", 1000}, {"cores", 2}, {"wait", 1}};
}

// Every parameter of this benchmark but the two axes is a count or a bound that must be >= 1.
inline std::uint64_t at_least_one(long long value, const char *name) {
    if (value < 1) {
        std::fprintf(stderr, "savina/cigsmok: %s must be >= 1 (got %lld)\n", name, value);
        std::abort();
    }
    return static_cast<std::uint64_t>(value);
}

// The arbiter's choices. Savina draws both from its PseudoRandom, a 16-bit LCG seeded with
// rounds * smokers -- deterministic already -- calling it twice per round: the smoker, then the
// period. That generator's low bit alternates, so every smoker draw is ODD and only the 100
// odd-numbered smokers of 200 are ever chosen (and every period is even); with smoker j placed on
// core (j + 1) % 2, it would also put every smoke on ONE core, the arbiter's. Here both draws come
// from qvo::mix over the round number, uniform over all the smokers -- a DEVIATION, recorded with
// the reproduction in cigsmok.md. The arbiter draws them and carries them in StartSmoking, as
// Savina's arbiter carries `busyWaitPeriod`.
inline constexpr std::uint64_t kChoice    = 0xc165c0c000000001ULL;
inline constexpr std::uint64_t kPeriod    = 0xc165c0c000000002ULL;
inline constexpr std::uint64_t kSmokeSeed = 0xc165c0c000000003ULL;
inline constexpr std::uint64_t kIdentity  = 0xc165c0c000000004ULL;
inline constexpr std::uint64_t kSmokeKey  = 0xc165c0c000000005ULL;
inline constexpr std::uint64_t kAckKey    = 0xc165c0c000000006ULL;
inline constexpr std::uint64_t kExitKey   = 0xc165c0c000000007ULL;

// The smoker chosen for round r (r from 0), in [0, smokers).
inline std::uint32_t smoker_of(std::uint64_t round, std::uint64_t smokers) noexcept {
    return static_cast<std::uint32_t>(qvo::mix(kChoice + round) % smokers);
}

// The busy-wait period of round r: `uniform[0, smoke) + 10` iterations, Savina's distribution.
inline std::uint32_t period_of(std::uint64_t round, std::uint64_t smoke) noexcept {
    return static_cast<std::uint32_t>(qvo::mix(kPeriod + round) % smoke + 10);
}

// The smoke itself: qvo::spin_work, the harness's one busy-work function, identical object code for
// every framework, seeded by the round so its result does not depend on who smokes it. Savina's
// busyWait spends each iteration on a Math.random() call -- a compare-and-swap on the one seed
// every thread shares, contended when two smokes overlap; here an iteration is one qvo::mix with
// nothing shared between threads: Savina's count, not its cost -- a DEVIATION (cigsmok.md).
inline std::uint64_t smoke_work(std::uint64_t round, std::uint32_t period) noexcept {
    return qvo::spin_work(qvo::mix(kSmokeSeed + round), static_cast<int>(period));
}

// A smoker's IDENTITY in the checksum: every term a smoker or the arbiter keeps about a smoker is
// weighted by it, so a message delivered to the WRONG smoker moves the sum as surely as a message
// lost or doubled. Smokers are numbered from 0.
inline std::uint64_t identity(std::uint32_t smoker) noexcept {
    return qvo::mix(kIdentity + smoker);
}

// What the smoker that received round r's StartSmoking (with its period) adds: who smoked, which
// round, and the smoke itself. `smoker` is the RECEIVER's own number, never one read from the
// message, so a misrouted StartSmoking is credited to the smoker that actually got it.
inline std::uint64_t smoke_term(std::uint32_t smoker, std::uint64_t round,
                                std::uint32_t period) noexcept {
    return identity(smoker) * qvo::mix(kSmokeKey + round) + smoke_work(round, period);
}

// What the arbiter adds for a StartedSmoking naming smoker n and round r -- for EVERY one it
// receives, including one that names a round that is not outstanding (see expected()). The
// smoker that sends it names ITSELF, never the smoker the arbiter drew, so a misrouted
// StartSmoking moves this term as well as the smoke term.
inline std::uint64_t ack_term(std::uint32_t smoker, std::uint64_t round) noexcept {
    return identity(smoker) * qvo::mix(kAckKey + round);
}

// What a smoker adds for every Exit it receives. It makes every Report a non-zero share named by
// its smoker -- a smoker the draw never chose smokes nothing, and without it a doubled Report of
// that smoker would add zero.
inline std::uint64_t exit_term(std::uint32_t smoker) noexcept {
    return identity(smoker) * qvo::mix(kExitKey);
}

// The checksum. Every round r has ONE designated smoker s(r) = smoker_of(r) and one period
// p(r) = period_of(r); the sum is
//
//     sum over r of  smoke_term(s(r), r, p(r)) + ack_term(s(r), r)
//   + sum over j of  exit_term(j)
//
// built as: each smoker adds smoke_term(its own number, r, the period it was sent) for every
// StartSmoking it receives and exit_term(its own number) for every Exit, and reports the total
// once, on Exit; the arbiter adds every report and ack_term(n, r) for every StartedSmoking (n, r)
// it receives. The value does not depend on the interleaving -- the choices are the round's, and
// the smoke of round r is the same whichever thread runs it -- and every kind of delivery is in it:
//
//   * a StartSmoking doubled, delivered to another smoker or carrying another period moves the
//     smoke term (the receiver credits ITS identity, a doubled one smokes twice, another period
//     is another smoke);
//   * a StartedSmoking doubled, or naming another smoker or another round, adds an ack term no
//     round expects. The arbiter folds EVERY StartedSmoking into its sum and, as Savina's arbiter
//     does, plays the next round on every one it receives until the last: a duplicate puts a
//     second round on the table and the run still completes -- every round chosen once, every
//     acknowledgement folded, the extra one included -- with a wrong sum (exit 1). A round still
//     in flight when the arbiter sends Exit is acknowledged before that smoker's Report: its
//     StartSmoking left before the Exit to the same smoker, its acknowledgement before the Report
//     from it, each pair on one ordered path;
//   * a Report doubled adds a smoker's share twice and, as the arbiter ends on the `smokers`-th
//     report, leaves another smoker's share out -- unless the duplicate is the last report of
//     all to arrive: it then comes after the terminal condition, outside the run, and the sum is
//     right. A smoker that receives a second Exit while it still runs reports twice the same way
//     (one that has ended -- qb, CAF -- receives nothing, as an exited actor in the reference).
//
// A LOST StartSmoking, StartedSmoking, Exit or Report -- or an Exit delivered to the wrong smoker,
// which leaves one smoker without its own -- cannot complete the run at all: the protocol waits for
// each, as Savina's does, and the stall is bounded by the driver's timeout (`tools/run.py
// --timeout`, reported as "timed out", verified false). So a drop is proved on the smoke too
// (cigsmok.md).
inline std::uint64_t expected(const qvo::Params &p) {
    const auto rounds  = at_least_one(p.get("rounds"), "rounds");
    const auto smokers = at_least_one(p.get("smokers"), "smokers");
    const auto smoke   = at_least_one(p.get("smoke"), "smoke");
    std::uint64_t acc  = 0;
    for (std::uint64_t r = 0; r < rounds; ++r) {
        const std::uint32_t s = smoker_of(r, smokers);
        acc += smoke_term(s, r, period_of(r, smoke)) + ack_term(s, r);
    }
    for (std::uint64_t j = 0; j < smokers; ++j) acc += exit_term(static_cast<std::uint32_t>(j));
    return acc;
}

// The report divides by this: one round -- a choice, a StartSmoking, a StartedSmoking, a smoke.
inline constexpr const char *kWorkUnit = "round";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("rounds"));
}

// Counted at the receivers: the arbiter counts its Start, every StartedSmoking and every Report;
// a smoker counts every StartSmoking and its Exit and carries the count in its Report. The
// arbiter's Start opens the run, `rounds` StartSmoking and `rounds` StartedSmoking play it, and
// `smokers` Exit and `smokers` Report end it: 2 * rounds + 2 * smokers + 1. The handshake each
// smoker sends the arbiter before the window (its Ready) is not counted. A duplicated
// StartedSmoking moves this count too.
//
// NOTHING is observed (Answer::observed stays empty, Spec::observed_at_least is not set): the
// round's smoker and period are the draw's, every count above is fixed, and the only thing the
// interleaving decides is how much the smoking overlaps the arbiter -- which is the time measured.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const auto rounds  = static_cast<std::uint64_t>(p.get("rounds"));
    const auto smokers = static_cast<std::uint64_t>(p.get("smokers"));
    return 2 * rounds + 2 * smokers + 1;
}

}  // namespace qvospec::savina::cigsmok

#endif  // QVOSPEC_SAVINA_CIGSMOK_H
