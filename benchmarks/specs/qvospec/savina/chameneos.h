// savina/chameneos — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Chameneos (Imam & Sarkar, AGERE 2014), benchmark 7, the first of the
// "concurrency" group. Deviations are recorded in benchmarks/savina/chameneos.md.

#ifndef QVOSPEC_SAVINA_CHAMENEOS_H
#define QVOSPEC_SAVINA_CHAMENEOS_H

#include <qvo/harness.h>

#include <cstdint>

namespace qvospec::savina::chameneos {

inline constexpr const char *kId = "savina/chameneos";

// The shape: `chameneos` creatures and ONE mall. A creature asks the mall to meet; the mall
// holds the first asker and, on the second, pairs the two -- it numbers the meeting k, tells
// both creatures the OTHER colour, and forgets them. A creature told of a meeting takes the
// complement colour and asks again. After `meetings` pairings the mall answers every further
// request with an exit, and the creature reports how many meetings it had. The mall is therefore
// a single mailbox written by every creature -- a fan-in hot spot -- that answers each request
// with a fan-out of two, which is exactly the shape counting (one writer) and big (all-to-all,
// no hot spot) do NOT cover.
//
// WHICH two creatures meet is the scheduler's decision and differs run to run, so a creature's
// colour sequence and its meeting count are not verifiable. What is verifiable is the mall's
// meeting numbering and the totals, and the checksum is built from those alone (see expected).
//
// `chameneos` -- creatures. Savina's own default, 100; no deviation.
// `meetings`  -- pairings before the mall closes. Savina's own default, 200 000; no deviation.
// `cores`     -- 1: mall and creatures on one thread; 2: the mall on one pinned thread and the
//                creatures on the other for the frameworks that place, so every request and
//                every reply crosses a core and the mall's mailbox is a real cross-core queue
//                with 100 writers.
// `wait`      -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"chameneos", 100}, {"meetings", 200000}, {"cores", 2}, {"wait", 1}};
}

// Savina's three colours and complement rule, kept for fidelity: the creature does the colour
// arithmetic on every meeting. Not part of the checksum -- see above.
enum Colour : std::uint8_t { kYellow = 0, kBlue = 1, kRed = 2 };

inline Colour complement(Colour a, Colour b) noexcept {
    if (a == b) return a;
    // The two differ: the third colour.
    return static_cast<Colour>(3 - a - b);
}

inline Colour initial_colour(std::uint32_t creature) noexcept {
    return static_cast<Colour>(creature % 3);
}

// Meeting k is announced to BOTH participants, each of which folds mix(k) into its own sum; the
// mall adds every creature's sum at exit and mixes the total meeting count in once. Every
// announcement, every exit and every count report is therefore in the checksum, and the value
// does not depend on who met whom.
inline std::uint64_t expected(const qvo::Params &p) {
    const long long m   = p.get("meetings");
    std::uint64_t   acc = 0;
    for (long long k = 0; k < m; ++k) acc += 2 * qvo::mix(static_cast<std::uint64_t>(k));
    return acc + qvo::mix(2 * static_cast<std::uint64_t>(m));
}

// The report divides by this: one pairing -- two requests in, two announcements out.
inline constexpr const char *kWorkUnit = "meeting";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("meetings"));
}

// The mall opens the window by telling every creature to start (c starts, inside the window --
// the same deal big's Start gets), every creature makes (its meetings + 1) requests, the last
// of which is answered with an exit, and reports once: c starts, 2m + c requests, 2m
// announcements, c exits, c reports. Counted at the receivers: a creature counts its start, its
// announcements and its exit and carries the count in its report; the mall counts requests
// and reports.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const auto m = static_cast<std::uint64_t>(p.get("meetings"));
    const auto c = static_cast<std::uint64_t>(p.get("chameneos"));
    return 4 * m + 4 * c;
}

}  // namespace qvospec::savina::chameneos

#endif  // QVOSPEC_SAVINA_CHAMENEOS_H
