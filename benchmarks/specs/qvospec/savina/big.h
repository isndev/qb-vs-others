// savina/big — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0).
//
// Savina reference: Big (Imam & Sarkar, AGERE 2014), benchmark 7 of the "micro" group.
// Deviations are recorded in benchmarks/savina/big.md.

#ifndef QVOSPEC_SAVINA_BIG_H
#define QVOSPEC_SAVINA_BIG_H

#include <qvo/harness.h>

namespace qvospec::savina::big {

inline constexpr const char *kId = "savina/big";

// The shape: `actors` actors, all-to-all. Each sends `pings` pings, one at a time, each to a
// peer chosen by its own deterministic sequence; the peer replies; the reply releases the next
// ping. Every actor is therefore BOTH a pinger with one request in flight and a ponger for
// everybody else, so every mailbox is a real many-producer queue with `actors` writers -- the
// contention case, which counting (one producer) and thread-ring (one token) never reach.
//
// `actors` -- Savina's own default, 120.
// `pings`  -- per actor. Savina's own default, 20 000 (2 400 000 round trips per repetition).
// `cores`  -- worker budget; actor a lives on core a % cores.
// `wait`   -- 1 = spin, 0 = park.
//
// A sink actor collects one `done` per actor and closes the window; it also opens it, by sending
// each actor its `start` once every actor has reported ready. Those `actors` start signals are
// inside the window and counted, because the alternative -- actors starting themselves -- has no
// single instant at which the window can be opened.
inline std::map<std::string, long long> params() {
    return {{"actors", 120}, {"pings", 20000}, {"cores", 2}, {"wait", 1}};
}

// Actor a's k-th target. A per-actor xorshift64 seeded from a, advanced once per ping, mapped
// onto the OTHER actors -- never onto a itself, so every ping is a real cross-actor message.
// Declared once so that an implementation and expected() cannot disagree about who was pinged.
class TargetSequence {
public:
    TargetSequence(std::uint32_t self, std::uint32_t actors) noexcept
        : state_(qvo::mix(static_cast<std::uint64_t>(self) + 1) | 1ULL)
        , self_(self)
        , actors_(actors) {}

    std::uint32_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        const auto r = static_cast<std::uint32_t>(state_ % (actors_ - 1));
        return r < self_ ? r : r + 1;
    }

private:
    std::uint64_t state_;
    std::uint32_t self_;
    std::uint32_t actors_;
};

// The value the PONGER computes and the pinger accumulates. It names the pinger, the ponger and
// the ping index, so a reply produced by the wrong actor -- or a ping delivered to a peer other
// than the one the sequence chose -- changes the sum. Needs actors < 2^20 and pings < 2^20.
inline std::uint64_t pong_value(std::uint32_t pinger, std::uint32_t ponger,
                                std::uint32_t k) noexcept {
    return qvo::mix((static_cast<std::uint64_t>(pinger) << 40) |
                    (static_cast<std::uint64_t>(ponger) << 20) | k);
}

inline std::uint64_t expected(const qvo::Params &p) {
    const auto    actors = static_cast<std::uint32_t>(p.get("actors"));
    const auto    pings  = static_cast<std::uint32_t>(p.get("pings"));
    std::uint64_t acc    = 0;
    for (std::uint32_t a = 0; a < actors; ++a) {
        TargetSequence seq(a, actors);
        for (std::uint32_t k = 0; k < pings; ++k) acc += pong_value(a, seq.next(), k);
    }
    return acc;
}

// The report divides by this: one ping and its pong.
inline constexpr const char *kWorkUnit = "round trip";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("actors")) *
           static_cast<std::uint64_t>(p.get("pings"));
}

// 2 x (actors x pings) + `actors` starts + `actors` dones.
//
// HOW AN IMPLEMENTATION COUNTS IT: the pinger counts, two per pong it receives -- the pong and
// the ping it answers, which the pong is the proof of -- plus its own start; the sink adds one
// per done. A ponger must NOT count the pings it receives: peers keep pinging an actor after it
// has reported done, so that count depends on a race (measured on the floor: 4 726 888 and
// 4 747 980 of 4 800 240 on two consecutive runs, same binary). ping-pong counts the same way.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return 2 * work_units(p) + 2 * static_cast<std::uint64_t>(p.get("actors"));
}

}  // namespace qvospec::savina::big

#endif  // QVOSPEC_SAVINA_BIG_H
