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

#include <cstddef>
#include <string>
#include <vector>

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
// 2^64 and block), both small enough for an actor's index to sit above every count in a digest
// term (below). Outside that, the run is refused with the harness's fatal exit 2 (qvo::refuse).
inline constexpr long long kMaxActors = 1LL << 24;
inline constexpr long long kMaxHops   = 1LL << 40;
inline void check(const qvo::Params &p) {
    const long long actors = p.get("actors");
    const long long hops   = p.get("hops");
    if (actors < 1 || actors >= kMaxActors || hops < 1 || hops >= kMaxHops)
        qvo::refuse("savina/thread-ring: need 1 <= actors < 2^24 and 1 <= hops < 2^40 (got actors="
                    + std::to_string(actors) + " hops=" + std::to_string(hops) + ")");
}

// THE VERIFIED ANSWER (Huly QB-1017). The token is a countdown and nothing else -- Savina's own
// shape. The sink injects `remaining = hops` at actor 0 and actor i hands `remaining - 1` to actor
// i + 1 (mod actors), so the hop of rank h (from 0) reaches actor h % actors with remaining
// hops - h. What a ring actor keeps of the tokens it received is a Tally: how many, and the sum
// of the countdowns they carried -- two independent adds, no multiply, no fold riding on the
// token. Each actor's tally is one 128-byte slot of a vector the body owns (no two actors,
// whatever thread runs them, write one line or one prefetched pair of lines); the body reads the
// slots once the framework is gone, OUTSIDE the window, and reduces them to the checksum below.
//
// Both numbers are fixed per actor by the parameters (expected_count, expected_sum): actor i
// receives exactly the ranks h = i, i + actors, i + 2 actors, ... below hops. So a delivery
// duplicated adds a count and a countdown twice; an actor the ring never reaches, or one that
// passes the token on without handling it, is short; a token delivered to the wrong actor puts a
// countdown into one tally that belongs to another, and shifts every later one. A CONSISTENT
// misroute -- every hop to i + 2 on an odd ring, which hands every actor as many tokens as it
// should get -- leaves every count right and still moves the sums: each actor receives another
// actor's ranks. A token lost never ends the run: it blocks, and the harness's deadline fails it
// (FAIRNESS.md section 0).
//
// A slot is 128 bytes on every target, not one 64-byte line: x86's adjacent-line (spatial)
// prefetcher and Apple M-series fetch lines in aligned 128-byte pairs, so two 64-byte slots of a
// pair written by two threads (actors i and i + 1 on different workers at cores=2) can bounce
// between their cores. With 64-byte slots Windows/MSVC measured the floor's 2c-spin cell +11.5 %
// and SObjectizer's +3.8 % against develop, every launch above (Huly QB-1017).
//
// The two numbers themselves are Counts. An adapter whose actors of two threads can never share a
// line may keep Counts inside the actor's own state, on the line its handler already reads, and
// hand them to the actor's Tally slot at teardown: qb, whose actor objects live in the arena of
// their own core's thread (qb/system/allocator/thread_arena.h). Measured on Windows/MSVC with the
// slot written on every hop instead: qb's 1c cells +7 % against develop, the hop touching one
// line more (Huly QB-1017).
struct Counts {
    std::uint64_t count{0};  // the tokens this actor's handler ran
    std::uint64_t sum{0};    // the countdowns they carried, wrapping
    void add(std::uint64_t remaining) noexcept {
        ++count;
        sum += remaining;
    }
};
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally : Counts {};

inline std::uint64_t expected_count(std::uint64_t index, std::uint64_t actors,
                                    std::uint64_t hops) noexcept {
    return index < hops ? (hops - 1 - index) / actors + 1 : 0;
}

// The sum of hops - (index + j * actors) for j below the count, in wrapping arithmetic -- the
// adapters' sums wrap the same way. count * (count - 1) / 2 is taken exactly modulo 2^64 by
// halving whichever factor is even.
inline std::uint64_t expected_sum(std::uint64_t index, std::uint64_t actors,
                                  std::uint64_t hops) noexcept {
    const std::uint64_t c = expected_count(index, actors, hops);
    if (c == 0) return 0;
    const std::uint64_t pairs = c % 2 == 0 ? (c / 2) * (c - 1) : c * ((c - 1) / 2);
    return c * (hops - index) - actors * pairs;
}

// One actor's tally in the checksum: its index, its count and its sum, mixed, so that a tally
// off in either number moves the total -- and an actor's tally carried by another moves it too.
inline std::uint64_t digest(std::uint64_t index, std::uint64_t count, std::uint64_t sum) noexcept {
    return qvo::mix(qvo::mix((index << 40) ^ count) ^ sum);
}

// What every adapter returns, computed after teardown from its tallies and the results its sink
// counted: the wrapping sum of the actors' digests, and every delivery the handlers ran.
inline std::uint64_t checksum(const std::vector<Tally> &tallies) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < tallies.size(); ++i)
        acc += digest(i, tallies[i].count, tallies[i].sum);
    return acc;
}
inline std::uint64_t delivered(const std::vector<Tally> &tallies, std::uint64_t results) noexcept {
    std::uint64_t n = results;
    for (const Tally &t : tallies) n += t.count;
    return n;
}

// The framework-free answer: the same digest over the closed form, O(actors).
inline std::uint64_t expected(const qvo::Params &p) {
    check(p);
    const auto    actors = static_cast<std::uint64_t>(p.get("actors"));
    const auto    hops   = static_cast<std::uint64_t>(p.get("hops"));
    std::uint64_t acc    = 0;
    for (std::uint64_t i = 0; i < actors; ++i)
        acc += digest(i, expected_count(i, actors, hops), expected_sum(i, actors, hops));
    return acc;
}

// The report divides by this: one hand-off of the token.
inline constexpr const char *kWorkUnit = "hop";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("hops"));
}

// `hops` token deliveries (the injection is the first) + one result to the sink -- COUNTED, not
// declared: the tallies' counts plus the results the sink's handler ran (delivered() above).
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("hops")) + 1;
}

}  // namespace qvospec::savina::thread_ring

#endif  // QVOSPEC_SAVINA_THREAD_RING_H
