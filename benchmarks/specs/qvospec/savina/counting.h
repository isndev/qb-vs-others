// savina/counting — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Counting (Imam & Sarkar, AGERE 2014), benchmark 2 of the "micro" group.
// Deviations are recorded in benchmarks/savina/counting.md.

#ifndef QVOSPEC_SAVINA_COUNTING_H
#define QVOSPEC_SAVINA_COUNTING_H

#include <qvo/harness.h>

namespace qvospec::savina::counting {

inline constexpr const char *kId = "savina/counting";

// The shape: ONE producer streams `messages` increments at ONE counter, one way, then asks for
// the total. Nothing waits for a reply until the very end, so this is the pure single-producer
// mailbox throughput case: how fast a framework can enqueue on one side and dequeue on the other
// with no round trip to hide behind. It is the complement of ping-pong, which measures a hop and
// never a queue depth above one.
//
// `messages`  -- increments. Savina's own default, 1 000 000; no deviation.
// `cores`     -- 1: both actors on one thread; 2: producer and counter on separate pinned
//                threads, so every increment crosses a core and the counter's mailbox is a real
//                cross-core queue.
// `wait`      -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"messages", 1000000}, {"cores", 2}, {"wait", 1}};
}

// The i-th increment carries i; the counter accumulates mix(i). A framework that coalesces two
// increments into one, or delivers the retrieve request AHEAD of an increment (the protocol
// requires it to arrive last), produces a different sum.
inline std::uint64_t expected(const qvo::Params &p) {
    const long long n   = p.get("messages");
    std::uint64_t   acc = 0;
    for (long long i = 0; i < n; ++i) acc += qvo::mix(static_cast<std::uint64_t>(i));
    return acc;
}

// The report divides by this: one increment delivered.
inline constexpr const char *kWorkUnit = "message";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("messages"));
}

// `messages` increments + one retrieve + one result.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("messages")) + 2;
}

}  // namespace qvospec::savina::counting

#endif  // QVOSPEC_SAVINA_COUNTING_H
