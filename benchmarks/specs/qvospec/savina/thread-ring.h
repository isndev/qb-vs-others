// savina/thread-ring — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0).
//
// Savina reference: ThreadRing (Imam & Sarkar, AGERE 2014), benchmark 3 of the "micro" group.
// Deviations are recorded in benchmarks/savina/thread-ring.md.

#ifndef QVOSPEC_SAVINA_THREAD_RING_H
#define QVOSPEC_SAVINA_THREAD_RING_H

#include <qvo/harness.h>

namespace qvospec::savina::thread_ring {

inline constexpr const char *kId = "savina/thread-ring";

// The shape: `actors` actors in a ring, ONE token, `hops` hand-offs. At any instant exactly one
// actor is runnable, so there is no parallelism to exploit and no queue ever holds more than one
// message: what is measured is the cost of a hand-off between two actors that are NOT the same
// two every time -- the scheduler has to find the next actor, and with more actors than fit in
// cache, the mailbox and the state the token lands on are cold.
//
// `actors` -- ring size. Savina's own default, 100.
// `hops`   -- hand-offs. DEVIATION FROM SAVINA: Savina's default is 100 000, which at the ~100 ns
//            per hop measured here is a 10 ms repetition and, like ping-pong's 40 000, noise with a
//            ranking on top. 1 000 000 is used, for the same reason ping-pong's count was raised.
//            `--param hops=100000` reproduces Savina's figure.
// `cores`  -- the worker budget. Actor i lives on core i % cores, so with cores=2 EVERY hop
//            crosses a core; that is the worst-case placement for a shard-per-core design and
//            deliberately the one published (benchmarks/savina/thread-ring.md says what each
//            framework's scheduler does with the same budget).
// `wait`   -- 1 = spin, 0 = park.
inline std::map<std::string, long long> params() {
    return {{"actors", 100}, {"hops", 1000000}, {"cores", 2}, {"wait", 1}};
}

// The token carries a countdown and an accumulator; each actor that receives it adds
// mix(remaining) before passing it on. Hop k (1-based; k = hops on injection) contributes
// mix(k), so a hop that is skipped, repeated, or delivered to an actor that forwards without
// touching the token changes the sum.
inline std::uint64_t expected(const qvo::Params &p) {
    const long long hops = p.get("hops");
    std::uint64_t   acc  = 0;
    for (long long k = hops; k >= 1; --k) acc += qvo::mix(static_cast<std::uint64_t>(k));
    return acc;
}

// The report divides by this: one hand-off of the token.
inline constexpr const char *kWorkUnit = "hop";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("hops"));
}

// `hops` token deliveries (the injection is the first) + one result to the sink.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("hops")) + 1;
}

}  // namespace qvospec::savina::thread_ring

#endif  // QVOSPEC_SAVINA_THREAD_RING_H
