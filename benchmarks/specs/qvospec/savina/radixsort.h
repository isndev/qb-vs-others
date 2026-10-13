// savina/radixsort — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Radix Sort (Imam & Sarkar, AGERE 2014), one of the "parallelism" benchmarks
// -- RadixSortConfig.java and RadixSortAkkaActorBenchmark.scala. Deviations are recorded in
// benchmarks/savina/radixsort.md.

#ifndef QVOSPEC_SAVINA_RADIXSORT_H
#define QVOSPEC_SAVINA_RADIXSORT_H

#include <qvo/harness.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace qvospec::savina::radixsort {

inline constexpr const char *kId = "savina/radixsort";

// The shape, Savina's own: a SOURCE draws `values` integers below 2^bits and sends them, one
// message each, down a LINEAR PIPELINE of `bits` sort stages -- stage k (from 0) looks at bit k --
// whose far end is a VALIDATION actor. A stage passes a value whose bit k is 0 on at once and
// HOLDS a value whose bit k is 1; once it has received all `values` it sends what it held, in
// arrival order. Each stage is a stable partition on its bit, so the chain is an LSD radix sort
// and the validation receives the values sorted. Every value crosses every stage: what is
// measured is sustained throughput along a chain of `bits` hand-offs, with a barrier at each
// stage that holds anything (a stage cannot send its held values before its last arrival).
//
// `values` -- N, the integers drawn. Savina's own default, 100 000; no deviation.
// `bits`   -- the stages, and the values' range below 2^bits. Savina's parameter is the maximum
//            value M = 2^60, and its stages test radix = M/2, M/4, ..., 1 -- a radix sort only
//            when M is a power of two, which every Savina configuration is. Here the exponent is
//            the parameter (a re-spelling, not a deviation): bits = 60 is Savina's M.
// `seed`   -- S, the seed of Savina's PseudoRandom. Savina's own default, 2 048.
// `cores`  -- the worker budget. For the frameworks that place (qb and the floor), the actors in
//            pipeline order -- the source, the stages, the validation -- are cut into `cores`
//            contiguous blocks (core_of below), so a value crosses a core once on its way.
// `wait`   -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"values", 100000}, {"bits", 60}, {"seed", 2048}, {"cores", 2}, {"wait", 1}};
}

// The parameters, checked by expected() before anything is built. Every stage holds up to
// `values` integers (Savina preallocates its orderingArray at that size), so `values` is bounded;
// `bits` is at least one stage and at most 62 (Savina's M is a positive long). Outside that, the
// run is refused with the harness's fatal exit 2 (qvo::refuse).
inline constexpr long long kMaxValues = 1LL << 22;
inline constexpr long long kMaxBits   = 62;
inline void check(const qvo::Params &p) {
    const long long values = p.get("values");
    const long long bits   = p.get("bits");
    if (values < 1 || values > kMaxValues || bits < 1 || bits > kMaxBits)
        qvo::refuse("savina/radixsort: need 1 <= values <= 2^22 and 1 <= bits <= 62 (got values="
                    + std::to_string(values) + " bits=" + std::to_string(bits) + ")");
}

// Savina's PseudoRandom (PseudoRandom.java), the source's generator: `value = (value * 1309 +
// 13849) & 65535`, and the source keeps `abs(nextLong()) % M`. Its values are 16-bit, so with
// Savina's M = 2^60 only the first 16 stages ever hold anything and the other 44 pass every value
// on at once -- the reference's own data, kept (benchmarks/savina/radixsort.md). The low 16 bits
// of a wrapping 64-bit product are a Java long's, whatever the seed's sign.
class Source {
public:
    Source(long long seed, long long bits) noexcept
        : _state(static_cast<std::uint64_t>(seed)), _mask((std::uint64_t{1} << bits) - 1) {}
    std::uint64_t next() noexcept {
        _state = (_state * 1309 + 13849) & 65535;
        return _state & _mask;  // abs() of a value in [0, 65535] is itself; % 2^bits is the mask
    }

private:
    std::uint64_t _state;
    std::uint64_t _mask;
};

// What a stage keeps of the values it received: how many, their wrapping sum and how many it held
// (bit k set) -- three adds, no multiply, nothing riding on a message. Each stage's tally is one
// 128-byte slot of a vector the body owns (no two stages, whatever thread runs them, write one
// line or one prefetched pair of lines: x86's adjacent-line prefetcher, Apple's 128-byte lines --
// thread-ring.h says what 64 bytes cost there); the body reads the slots once the framework is
// gone. An adapter whose actors of two threads can never share a line keeps the Counts in the
// actor's own state and hands them to the slot at teardown (qb: an actor lives in its core
// thread's arena), as thread-ring's qb adapter does.
struct Counts {
    std::uint64_t count{0};  // the values this stage's handler ran
    std::uint64_t sum{0};    // their wrapping sum
    std::uint64_t held{0};   // the ones it held for the flush (bit k set)
};
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally : Counts {};

// A stage that received more values than the source draws breaks the protocol in a way no sum can
// report in time -- it already flushed at its `values`-th -- so it stops the run: qvo::fail(), exit
// 1 and a failure document, from the thread that saw it (never std::abort -- harness.h).
[[noreturn]] inline void overflow(const char *who) {
    qvo::fail(std::string("savina/radixsort: protocol violated -- ") + who
              + " received more values than the source draws");
}

// One sort stage's decision, Savina's SortActor step for step, written once so the test in every
// handler is the same source: take() counts the value and answers whether it goes on now (bit k
// is 0) or is held; complete() is true after the `values`-th, when the adapter sends what it held,
// in arrival order -- AFTER a value take() passed in the same handler, as the reference does. The
// Counts it keeps are wherever the adapter puts them: the qb actor's own member, the slot itself
// for the others (Counts above). What a stage holds is the adapter's too: qb, CAF and the floor
// hold the integer, SObjectizer the message it received, which it re-sends as the reference
// re-sends its ValueMessage; each reserves room for `values` before the window, Savina's
// orderingArray(numValues).
class Stage {
public:
    Stage() noexcept = default;
    Stage(std::uint32_t index, std::uint64_t values, Counts &counts) noexcept
        : _radix(std::uint64_t{1} << index), _values(values), _counts(&counts) {}

    bool take(std::uint64_t value) noexcept {
        Counts &c = *_counts;
        if (++c.count > _values) overflow("a sort stage");
        c.sum += value;
        if ((value & _radix) == 0) return true;
        ++c.held;
        return false;
    }
    bool complete() const noexcept { return _counts->count == _values; }

private:
    std::uint64_t _radix{0};
    std::uint64_t _values{0};
    Counts       *_counts{nullptr};
};

// The validation actor's whole work inside the window: the value goes to the next position of a
// sequence the body owns (one store; Savina's ValidationActor compares and adds), and take()
// answers whether it was the last. The sequence is checked against the sorted input AFTER the
// window (checksum below), so an order the reference's one comparison would let through -- two
// values exchanged across a third -- cannot.
class Validation {
public:
    Validation() noexcept = default;
    Validation(std::uint64_t *sequence, std::uint64_t values) noexcept
        : _sequence(sequence), _values(values) {}
    bool take(std::uint64_t value) noexcept {
        if (_count >= _values) overflow("the validation");
        _sequence[_count++] = value;
        return _count == _values;
    }
    std::uint64_t count() const noexcept { return _count; }

private:
    std::uint64_t *_sequence{nullptr};
    std::uint64_t  _values{0};
    std::uint64_t  _count{0};
};

// Where an adapter whose actors may share a line keeps the validation (CAF, SObjectizer, the
// floor): a line of its own the body owns. qb keeps it in the actor, like a stage's Counts.
struct alignas(kTallyLine) ValidationSlot {
    Validation validation;
};

// THE VERIFIED ANSWER. Two parts, both fixed by the parameters:
//
//   * every stage's tally: it receives all `values` (every value crosses every stage), their sum
//     is the input's, and it holds exactly the values whose bit k is set -- digest(k, ...) binds
//     the three to the stage's index, so a tally carried by another stage moves the total;
//   * the sequence the validation received, position by position: an LSD radix sort of stable
//     partitions IS the stable sort, so the i-th value received is the i-th of the sorted input.
//
// So a value delivered twice reaches a stage as its (values+1)-th and fails the run (overflow),
// or reaches the validation as one position too many; a stage that passes on a value it should
// have held, flushes its held values out of order, or is routed around altogether, leaves the
// sequence unsorted and its tally short; a value altered in flight moves the sums and the
// sequence. A value LOST -- or sent past the next stage -- leaves a stage short of its
// `values`-th: it never flushes, the validation never completes, the run BLOCKS and the harness's
// deadline fails it (benchmarks/savina/radixsort.md, "What blocks, and what moves the sum").
// Stages wired in the wrong bit order leave every tally right -- each still partitions its own
// bit -- and only the sequence sees it: the reason the sequence is checked whole.
inline std::uint64_t stage_digest(std::uint64_t index, const Counts &c) noexcept {
    return qvo::mix(qvo::mix(qvo::mix(0x7ad15e0000000000ULL ^ (index << 40) ^ c.count) ^ c.sum)
                    ^ c.held);
}
inline std::uint64_t position_digest(std::uint64_t position, std::uint64_t value) noexcept {
    return qvo::mix(qvo::mix(0x7ad15e5e00000000ULL + position) ^ value);
}

// What every adapter returns, computed after teardown: the stages' digests and the sequence's.
inline std::uint64_t checksum(const std::vector<Tally> &tallies,
                              const std::vector<std::uint64_t> &sequence) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t k = 0; k < tallies.size(); ++k) acc += stage_digest(k, tallies[k]);
    for (std::size_t i = 0; i < sequence.size(); ++i) acc += position_digest(i, sequence[i]);
    return acc;
}

// Every delivery the handlers ran: each stage's count, the values the validation took, and the
// source's one Start.
inline std::uint64_t delivered(const std::vector<Tally> &tallies, std::uint64_t validated,
                               std::uint64_t starts) noexcept {
    std::uint64_t n = validated + starts;
    for (const Tally &t : tallies) n += t.count;
    return n;
}

// The input, drawn exactly as the source draws it.
inline std::vector<std::uint64_t> input(const qvo::Params &p) {
    const auto              values = static_cast<std::size_t>(p.get("values"));
    Source                  source(p.get("seed"), p.get("bits"));
    std::vector<std::uint64_t> v(values);
    for (auto &x : v) x = source.next();
    return v;
}

// The tally every stage must end with, from the input alone.
inline Counts expected_counts(const std::vector<std::uint64_t> &in, std::uint64_t index) noexcept {
    Counts c;
    c.count = in.size();
    for (const std::uint64_t v : in) {
        c.sum += v;
        c.held += (v >> index) & 1;
    }
    return c;
}

// The framework-free answer: the same digests over the closed form -- the input's tallies per
// stage, and the input sorted.
inline std::uint64_t expected(const qvo::Params &p) {
    check(p);
    const auto    bits = static_cast<std::uint64_t>(p.get("bits"));
    auto          in   = input(p);
    std::uint64_t acc  = 0;
    for (std::uint64_t k = 0; k < bits; ++k) acc += stage_digest(k, expected_counts(in, k));
    std::sort(in.begin(), in.end());
    for (std::size_t i = 0; i < in.size(); ++i) acc += position_digest(i, in[i]);
    return acc;
}

// The report divides by this: one value handed from one actor of the chain to the next -- from the
// source to the first stage, from each stage to the next, from the last to the validation.
inline constexpr const char *kWorkUnit = "message";
inline std::uint64_t work_units(const qvo::Params &p) {
    return static_cast<std::uint64_t>(p.get("values")) * (static_cast<std::uint64_t>(p.get("bits")) + 1);
}

// `values` deliveries at each of the `bits` stages and at the validation, plus the source's one
// Start -- COUNTED where they were received (delivered() above), never declared.
inline std::uint64_t expected_messages(const qvo::Params &p) { return work_units(p) + 1; }

// The placement of the frameworks that place: the actors in pipeline order (position 0 the source,
// 1 + k stage k, bits + 1 the validation) cut into `cores` contiguous blocks.
inline std::uint32_t positions(std::uint64_t bits) noexcept {
    return static_cast<std::uint32_t>(bits + 2);
}
inline std::uint32_t core_of(std::uint32_t position, std::uint32_t count, int cores) noexcept {
    const auto c = static_cast<std::uint64_t>(cores < 1 ? 1 : cores);
    return static_cast<std::uint32_t>(std::uint64_t{position} * c / count);
}

}  // namespace qvospec::savina::radixsort

#endif  // QVOSPEC_SAVINA_RADIXSORT_H
