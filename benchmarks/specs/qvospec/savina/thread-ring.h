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

#include <string>

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

// The parameters, checked by expected() before anything is built: a ring of at least one actor
// and a token that makes at least one hop (one injected with no hop left would count down from
// 2^64 and block), both small enough for an actor's index to sit above every hop count in a term
// (below). Outside that, the run is refused with the harness's fatal exit 2 (qvo::refuse).
inline constexpr long long kMaxActors = 1LL << 24;
inline constexpr long long kMaxHops   = 1LL << 40;
inline void check(const qvo::Params &p) {
    const long long actors = p.get("actors");
    const long long hops   = p.get("hops");
    if (actors < 1 || actors >= kMaxActors || hops < 1 || hops >= kMaxHops)
        qvo::refuse("savina/thread-ring: need 1 <= actors < 2^24 and 1 <= hops < 2^40 (got actors="
                    + std::to_string(actors) + " hops=" + std::to_string(hops) + ")");
}

// The token carries a countdown and an accumulator; the ring actor at index i that receives it
// with `remaining` = k adds term(i, k) before passing it on, and every adapter folds through THIS
// function. The injection hands k = hops to actor 0 and actor i passes k - 1 to actor i + 1 (mod
// actors), so the hop of rank h (from 0) is folded by actor h % actors with k = hops - h: the
// expected sum names, for every hop, WHICH actor folds it. A term skipped or folded twice (an
// actor that decrements without adding, or adds without decrementing), a token delivered to the
// wrong ring actor, an actor the ring routes around, a token one actor forwards untouched -- each
// puts a term under another actor's index or another k, and every later term with it: the sum
// moves (Huly QB-1017). A token DUPLICATED gives two tokens, each carrying a right total: the count
// below is what sees it. A token lost never ends the run: it blocks, and the harness's deadline
// fails it (FAIRNESS.md section 0).
inline std::uint64_t term(std::uint64_t index, std::uint64_t remaining) noexcept {
    return qvo::mix((index << 40) ^ remaining);
}

inline std::uint64_t expected(const qvo::Params &p) {
    check(p);
    const auto    actors = static_cast<std::uint64_t>(p.get("actors"));
    std::uint64_t acc    = 0;
    std::uint64_t index  = 0;
    for (auto k = static_cast<std::uint64_t>(p.get("hops")); k >= 1; --k) {
        acc += term(index, k);
        if (++index == actors) index = 0;
    }
    return acc;
}

// The report divides by this: one hand-off of the token.
inline constexpr const char *kWorkUnit = "hop";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("hops"));
}

// `hops` token deliveries (the injection is the first) + one result to the sink -- COUNTED, not
// declared: every ring actor counts the tokens its handler ran and the sink the results its
// handler ran, and the adapter reports the sum of those counts, collected once the framework has
// torn down (a ring actor hands its count over from its destructor or its finish hook, outside
// the window). A token delivered twice -- even the last one, whose copy's sum is right -- is one
// delivery too many (Huly QB-1017).
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("hops")) + 1;
}

}  // namespace qvospec::savina::thread_ring

#endif  // QVOSPEC_SAVINA_THREAD_RING_H
