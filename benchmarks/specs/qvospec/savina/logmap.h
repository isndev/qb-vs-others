// savina/logmap — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply to every sum ACROSS
// messages here; inside one series, whose terms are strictly sequential, the fold is ordered on
// purpose (see chain_step).
//
// Savina reference: Logistic Map Series (Imam & Sarkar, AGERE 2014), LogisticMapConfig.java and
// LogisticMapAkkaManualStashActorBenchmark.scala -- the "manual stash" variant, whose series
// worker asks its rate computer with a plain message and holds everything else until the answer
// arrives. Deviations are recorded in benchmarks/savina/logmap.md.

#ifndef QVOSPEC_SAVINA_LOGMAP_H
#define QVOSPEC_SAVINA_LOGMAP_H

#include <qvo/harness.h>

#include <bit>
#include <cstdint>

namespace qvospec::savina::logmap {

inline constexpr const char *kId = "savina/logmap";

// The shape: a master, `series` series workers and `series` rate computers -- computer i holds
// the rate of series i and serves worker i alone (Savina: `computers(i % numComputers)` with as
// many computers as workers). The master sends every worker `terms` NextTerm requests ALL AT
// ONCE, interleaved across the workers exactly as the reference's loop does (term by term, worker
// by worker), then one GetTerm each. A worker that is asked for the next term sends its current
// term to its computer, which answers x' = rate * x * (1 - x) to the SENDER; until that answer
// arrives the worker holds every other request, and it answers GetTerm only once nothing is held
// and nothing is in flight. When every worker has answered, the master tells each computer to
// stop and each reports what it served (Savina's StopMessage, answered).
//
// So the benchmark is `series` independent chains of `terms` strictly sequential request/reply
// round trips, started by a fan-out of `series * terms` messages that lands in deep mailboxes:
// the latency of one ask-and-answer, multiplied by the length of a chain, with the parallelism
// of `series` chains -- and what a framework pays to deliver a burst it cannot act on yet.
//
// A NextTerm carries nothing, so "holding" one is counting it: every adapter keeps the held
// requests as a count, where the reference stashes the message objects and re-sends each to
// itself (a second delivery of every held request) -- a deviation recorded on the page.
//
// `terms`  -- terms per series. Savina's own default, 25 000; no deviation.
// `series` -- series workers, and as many rate computers. Savina's own default, 10; no deviation.
// `cores`  -- 1: everything on one thread; 2: the master on core 0 and series i -- its worker AND
//             its computer, which talk to nobody else -- on core (1 + i) % cores, for the
//             frameworks that place, so the round trips stay on their core and the chains split
//             5 / 5; the pools place freely.
// `wait`   -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
//
// Savina's start rate (3.46) and rate increment (0.0025) are doubles and qvo parameters are
// integers: both are fixed here, at Savina's defaults (see rate_of / start_of).
inline std::map<std::string, long long> params() {
    return {{"terms", 25000}, {"series", 10}, {"cores", 2}, {"wait", 1}};
}

// The rate of series i and its first term. Savina computes `3.46 + i * 0.0025` and
// `i * 0.0025` in doubles; written as a multiply feeding an add, the first is exactly the shape a
// compiler may contract into one fused multiply-add on a target that has one (arm64 always, with
// clang's default -ffp-contract=on), which skips a rounding and changes the last bit of the rate
// -- and with it every term of the series -- on one host and not another. So both are written as
// ONE correctly rounded division of exact integers: 3.46 = 1384 / 400 and 0.0025 = 1 / 400. That
// is the IEEE double nearest to Savina's decimal value on every compiler and every target; it can
// differ from the JVM's two-rounding result in the last bit, for some i -- recorded on the page.
inline double rate_of(std::uint64_t i) noexcept {
    return static_cast<double>(1384 + i) / 400.0;
}
inline double start_of(std::uint64_t i) noexcept {
    return static_cast<double>(i) / 400.0;
}

// One step of the logistic map, in Savina's evaluation order: (rate * x) * (1 - x). No product
// here feeds an addition, so there is nothing a compiler may fuse, and each of the three
// operations is one correctly rounded IEEE operation on every target qvo builds for (SSE2 on
// x86-64 -- no -march, no fast-math, CMakeLists.txt; NEON on arm64): the same bits everywhere.
inline double next_term(double x, double rate) noexcept {
    const double grown = rate * x;
    const double room  = 1.0 - x;
    return grown * room;
}

// A rounding barrier for the reference loop below. In every adapter a term crosses a MESSAGE
// between the multiplication that produces it and the subtraction that consumes it, so it is
// always a rounded double in memory; in a plain loop the two would meet in one function, where a
// compiler contracting across statements (g++'s default -ffp-contract=fast, on a target with
// FMA) could compute 1 - (p * q) fused. The volatile round trip gives the reference the same
// rounding the messages give the adapters. It runs once per repetition, outside the window.
inline double settle(double x) noexcept {
    volatile double v = x;
    return v;
}

// Who a term belongs to: odd, so a multiplication by it is a bijection, and different for every
// series.
inline std::uint64_t identity(std::uint64_t i) noexcept { return qvo::mix(i + 1) | 1; }

// A series' terms are strictly sequential -- one request in flight per series, ever -- so the
// worker folds them IN ORDER, every term it receives, by its exact bit pattern (FNV-1a's 64-bit
// step over one 64-bit word). An ordered fold, not a sum: a logistic orbit at these rates settles
// on a period-4 cycle, so a run that computed four terms too few would end on the same final
// term, and a fold over the final term alone would verify it. Here a term dropped, added or
// answered out of turn changes the fold. The seed is the series' identity, so two series that
// exchanged a term change both folds.
inline constexpr std::uint64_t kFoldPrime = 0x100000001b3ULL;
inline std::uint64_t chain_seed(std::uint64_t i) noexcept { return identity(i); }
inline std::uint64_t chain_step(std::uint64_t chain, double term) noexcept {
    return (chain ^ std::bit_cast<std::uint64_t>(term)) * kFoldPrime;
}

// What the master adds when worker i answers GetTerm with its fold, and when computer i reports
// how many requests it served (exactly `terms`: a request served twice or never changes it).
inline std::uint64_t series_key(std::uint64_t i, std::uint64_t chain) noexcept {
    return qvo::mix(chain + identity(i));
}
inline std::uint64_t computer_key(std::uint64_t i, std::uint64_t served) noexcept {
    return qvo::mix(served * identity(i) + 0x636f6d7075746572ULL);  // "computer"
}

// The fold of series i after `terms` steps -- the reference every worker's answer is held to.
inline std::uint64_t series_chain(std::uint64_t i, std::uint64_t terms) noexcept {
    const double  rate  = rate_of(i);
    double        x     = start_of(i);
    std::uint64_t chain = chain_seed(i);
    for (std::uint64_t k = 0; k < terms; ++k) {
        x     = settle(next_term(x, rate));
        chain = chain_step(chain, x);
    }
    return chain;
}

// The master folds, as wrapping sums, series_key(i, chain) for every worker's answer and
// computer_key(i, served) for every computer's report. Series 0 starts at 0 and stays there
// (Savina's own parameters: 0 * anything is 0) and is verified like the others -- its fold still
// counts its `terms` steps.
inline std::uint64_t expected(const qvo::Params &p) {
    const auto    s   = static_cast<std::uint64_t>(p.get("series"));
    const auto    t   = static_cast<std::uint64_t>(p.get("terms"));
    std::uint64_t acc = 0;
    for (std::uint64_t i = 0; i < s; ++i)
        acc += series_key(i, series_chain(i, t)) + computer_key(i, t);
    return acc;
}

// The report divides by this: one term -- a NextTerm delivered, the request to the computer and
// its answer.
inline constexpr const char *kWorkUnit = "term";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("series")) * static_cast<std::uint64_t>(p.get("terms"));
}

// Counted at the receivers, inside the window: a worker receives `terms` NextTerm, `terms`
// answers and one GetTerm (2t + 1); a computer `terms` requests and one stop (t + 1); the master
// `series` answers and `series` reports. The readiness handshake that opens the window is outside
// it and not counted, and so is the teardown after the last report.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const auto s = static_cast<std::uint64_t>(p.get("series"));
    const auto t = static_cast<std::uint64_t>(p.get("terms"));
    return s * (3 * t + 4);
}

// Reported beside the cell, NOT asserted (FAIRNESS.md section 0): how many NextTerm requests
// found their worker waiting for an answer and were held. It depends on how the master's burst
// interleaved with the chains -- every request but the first of each series when the burst is
// delivered before any answer, fewer when answers overtake its tail -- and not on the amount of
// work, which is `terms` requests to the computer per series either way.
inline constexpr const char *kHeld = "held";

}  // namespace qvospec::savina::logmap

#endif  // QVOSPEC_SAVINA_LOGMAP_H
