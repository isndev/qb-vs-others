// savina/ping-pong — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header. The expected checksum and the expected
// message count are computed here, by plain arithmetic, with no framework linked. An
// implementation therefore cannot define its own notion of "correct", and a framework that drops,
// duplicates or coalesces a single message produces a different checksum and is reported as
// FAILED with no timing at all (FAIRNESS.md section 0).
//
// Savina reference: PingPong (Imam & Sarkar, AGERE 2014), benchmark 1 of the "micro" group.
// Deviations from Savina's own parameters are recorded in benchmarks/savina/ping-pong.md.

#ifndef QVOSPEC_SAVINA_PING_PONG_H
#define QVOSPEC_SAVINA_PING_PONG_H

#include <qvo/harness.h>

namespace qvospec::savina::ping_pong {

inline constexpr const char *kId = "savina/ping-pong";

// `messages` — round trips.
//
//   DEVIATION FROM SAVINA: Savina's default is 40 000. Measured here, 40 000 round trips complete
//   in ~10 ms with a run-to-run IQR near 20 %, which is wider than any framework difference this
//   benchmark could report -- the measurement would be noise with a ranking printed on top of it.
//   Savina's figure is calibrated for JVM warm-up, not for a native binary. The default is raised
//   to 1 000 000, which is also what qb's own ping-pong benchmark uses. Pass
//   `--param messages=40000` to reproduce Savina's figure exactly.
//
// `cores` — how many OS threads the framework is permitted for this workload. Mapped to each
//   framework's nearest configuration, documented per framework in the .md spec. It is a declared
//   parameter rather than a hard-coded choice because "two actors on one thread" and "two actors
//   on two threads" are different questions, and measuring only the one that flatters a
//   shard-per-core design is exactly what FAIRNESS.md 1.6 exists to prevent.
//
// `wait` — 1 = spin, 0 = park. THE SINGLE LARGEST CONFIGURATION LEVER IN THIS REPOSITORY.
//
//   A runtime whose workers busy-spin wins any latency benchmark against one that parks on a
//   condition variable, by a margin that dwarfs every architectural difference being compared.
//   qb's own benchmarks run with `setLatency(0)`, i.e. spinning. Measuring qb spinning against
//   CAF parking, and reporting the ratio as an architectural result, would be the single most
//   effective way to rig this comparison -- so it is a declared axis and BOTH values are always
//   measured and always published.
//
//   Every framework here can do both:
//     qb          setLatency(qb::duration::zero())  vs  a non-zero park interval
//     CAF         caf.work-stealing.*-poll-attempts / relaxed-sleep-duration
//     SObjectizer the mpsc queue lock factory (combined = spin-then-park, simple = park)
//     baseline    a spinning ring vs a condition-variable handoff
inline std::map<std::string, long long> params() {
    return {{"messages", 1000000}, {"cores", 2}, {"wait", 1}};
}

// The accumulator is a WRAPPING SUM of mix(seq), not an XOR.
//
// That is deliberate, and it is the difference between a checksum that catches a defect and one
// that does not: XOR is self-cancelling, so a message delivered twice leaves it unchanged. A sum
// is sensitive to drops AND to duplicates, while staying insensitive to arrival ORDER -- which
// matters because the fan-in benchmarks in this suite have no deterministic order and must use
// the same reduction as the sequential ones.
inline std::uint64_t expected(const qvo::Params &p) {
    const long long n   = p.get("messages");
    std::uint64_t   acc = 0;
    for (long long i = 0; i < n; ++i) acc += qvo::mix(static_cast<std::uint64_t>(i));
    return acc;
}

// One ping and one pong per round trip.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return 2ull * static_cast<std::uint64_t>(p.get("messages"));
}

}  // namespace qvospec::savina::ping_pong

#endif  // QVOSPEC_SAVINA_PING_PONG_H
