// savina/piprecision — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum is computed with no
// framework linked (FAIRNESS.md section 0).
//
// Savina reference: Precise Pi Computation, `piprecision` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- PiPrecisionConfig.java for the parameters and calculateBbpTerm,
// PiPrecisionAkkaActorBenchmark.scala for the semantics. Deviations are recorded in
// benchmarks/savina/piprecision.md.

#ifndef QVOSPEC_SAVINA_PIPRECISION_H
#define QVOSPEC_SAVINA_PIPRECISION_H

#include <qvo/harness.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qvospec::savina::piprecision {

inline constexpr const char *kId = "savina/piprecision";

// The shape, Savina's own: a MASTER and `workers` WORKERS, all created before the window, compute
// pi to `precision` decimal places with the Bailey-Borwein-Plouffe series. The master hands out
// term indices -- first min(precision, 10 x workers) of them round-robin, then one more to each
// worker that answers -- and a worker answers each with the term, a number of `precision` decimal
// places: the MESSAGE IS HEAVY, about a kilobyte on average and two at the first terms (the field
// of piprecision.md). The master adds every term into its sum, stops handing out work once a term
// at or below 10^-precision comes back, and the run ends when every term it handed out is back.
// The fan-out / fan-in of savina/trapezoid with heavy messages and a master that keeps the
// workers fed: what a framework decides is how a payload crosses from a worker to the master.
//
// `precision` -- decimal places, Savina's `scale`. Savina's own default, 5 000.
// `workers`   -- Savina's own default, 20; worker w on core w % cores where the framework places.
// `form`      -- how an answer's payload travels, a declared axis (piprecision.md, "The payload"):
//                0 = in a heap buffer the worker fills and MOVES into the message, the message
//                carrying its pointer -- every framework; 1 = copied into the message's own
//                storage, a variable-length tail behind the event -- qb's Pipe::allocated_push;
//                CAF, SObjectizer and the floor have no variable-length inline message and report
//                it not applicable. Each adapter's table cell runs the faster of its forms as
//                measured on the quiet host, and its main() says which. The result document
//                records the value.
// `cores`     -- worker-thread budget. `wait` -- 1 = spin, 0 = park.
inline std::map<std::string, long long> params() {
    return {{"precision", 5000}, {"workers", 20}, {"form", 0}, {"cores", 2}, {"wait", 1}};
}

// `form`, which is 0 or 1: true for the payload inline in the message.
inline bool inline_payload(const qvo::Params &p) {
    const long long v = p.get("form");
    if (v != 0 && v != 1)
        qvo::refuse("savina/piprecision: form must be 0 or 1 (got " + std::to_string(v) + ")");
    return v == 1;
}

// The parameters, checked before anything is built: a precision small enough for a term's payload
// to stay far inside any framework's message limits (qb's cross-core ring holds an event of up to
// ~64 KiB; a term here is at most 20 KiB) and a pool that exists. Outside that, the run is refused
// with the harness's fatal exit 2 (qvo::refuse).
inline constexpr long long kMaxPrecision = 50000;
inline constexpr long long kMaxWorkers   = 1LL << 12;

struct Config {
    std::uint32_t precision{0};
    std::uint32_t workers{0};
    std::uint32_t batch{0};  // the terms handed out at the start: Savina's min(scale, 10 x workers)
};

inline Config config(const qvo::Params &p) {
    const long long precision = p.get("precision");
    const long long workers   = p.get("workers");
    if (precision < 1 || precision > kMaxPrecision || workers < 1 || workers > kMaxWorkers)
        qvo::refuse("savina/piprecision: need 1 <= precision <= " + std::to_string(kMaxPrecision)
                    + " and 1 <= workers <= " + std::to_string(kMaxWorkers) + " (got precision="
                    + std::to_string(precision) + " workers=" + std::to_string(workers) + ")");
    (void)inline_payload(p);
    Config c;
    c.precision = static_cast<std::uint32_t>(precision);
    c.workers   = static_cast<std::uint32_t>(workers);
    c.batch     = std::min(c.precision, 10 * c.workers);
    return c;
}

// ---------------------------------------------------------------------------------------------
// Exact arithmetic. Savina computes with java.math.BigDecimal at `scale` decimal places, rounding
// HALF_EVEN at each division. A BigDecimal at that scale is an integer U (its unscaled value) worth
// U x 10^-scale, so every operation of calculateBbpTerm is an operation on integers:
//
//     4 / (8k+1) at the scale   ->  round_half_even(4 x 10^scale / (8k+1))
//     a - b                     ->  U(a) - U(b)                       (exact)
//     x / 16^k at the scale     ->  round_half_even(U(x) / 2^(4k))    (a shift)
//     sum += term               ->  U(sum) + U(term)                  (exact)
//
// Here U is a binary integer of 64-bit limbs, little-endian, and the computation is EXACTLY
// Savina's, digit for digit: the same terms, the same rounding, the same sum. The sum is a sum of
// integers -- its value does not depend on the order the terms arrive in, which is what makes the
// answer exact (piprecision.md, "The verified answer"). The divisors 8k + 1 .. 8k + 6 stay below
// 2^32 for every term this spec allows, so the long division runs on 32-bit halves of a limb and
// needs no 128-bit arithmetic on any compiler.
// ---------------------------------------------------------------------------------------------

using Limb = std::uint64_t;

inline constexpr Limb kLow32 = 0xffffffffULL;

// v *= m (m < 2^32), growing v by a limb when the product carries out of it.
inline void mul_small(std::vector<Limb> &v, std::uint32_t m) {
    std::uint64_t carry = 0;
    for (Limb &x : v) {
        std::uint64_t t = (x & kLow32) * m + carry;
        const Limb    lo = t & kLow32;
        t                = (t >> 32) + (x >> 32) * m;
        x                = ((t & kLow32) << 32) | lo;
        carry            = t >> 32;
    }
    if (carry != 0) v.push_back(carry);
}

// v += 1, from limb 0; the caller guarantees room for the carry.
inline void increment(Limb *v, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n && ++v[i] == 0; ++i) {
    }
}

// q = round_half_even(num / d), num and q of n limbs, 0 < d < 2^32. Java's BigDecimal.divide
// (divisor, scale, HALF_EVEN) of two values at one scale, on their unscaled integers.
inline void div_round(const Limb *num, std::size_t n, std::uint32_t d, Limb *q) noexcept {
    std::uint64_t r = 0;  // r < d < 2^32, so each partial dividend fits 64 bits
    for (std::size_t i = n; i-- > 0;) {
        std::uint64_t       cur = (r << 32) | (num[i] >> 32);
        const std::uint64_t qh  = cur / d;
        r                       = cur % d;
        cur                     = (r << 32) | (num[i] & kLow32);
        const std::uint64_t ql  = cur / d;
        r                       = cur % d;
        q[i]                    = (qh << 32) | ql;
    }
    if (2 * r > d || (2 * r == d && (q[0] & 1) != 0)) increment(q, n);
}

// a -= b over n limbs; returns the borrow out (true when b > a).
inline bool sub_in_place(Limb *a, const Limb *b, std::size_t n) noexcept {
    bool borrow = false;
    for (std::size_t i = 0; i < n; ++i) {
        const Limb x = a[i];
        const Limb y = b[i] + static_cast<Limb>(borrow);
        borrow       = y < b[i] || x < y;
        a[i]         = x - y;
    }
    return borrow;
}

inline std::size_t significant(const Limb *v, std::size_t n) noexcept {
    while (n > 0 && v[n - 1] == 0) --n;
    return n;
}

// The limbs round_half_even(t / 2^shift) can occupy, the rounding carry included -- the room a
// caller provides to shr_round() below.
inline std::size_t shr_room(std::size_t n, std::uint64_t shift) noexcept {
    const std::uint64_t ls = shift / 64;
    return (ls < n ? n - static_cast<std::size_t>(ls) : 0) + 1;
}

// out[0 .. shr_room(n, shift)) = round_half_even(t / 2^shift); returns its significant limbs. The
// division by 16^k at the scale, a shift on the unscaled integer.
inline std::size_t shr_round(const Limb *t, std::size_t n, std::uint64_t shift, Limb *out) noexcept {
    const std::uint64_t ls   = shift / 64;
    const unsigned      bs   = static_cast<unsigned>(shift % 64);
    const std::size_t   room = shr_room(n, shift);
    for (std::size_t i = 0; i + 1 < room; ++i) {
        const std::size_t j = i + static_cast<std::size_t>(ls);
        out[i] = bs == 0 ? t[j] : (t[j] >> bs) | (j + 1 < n ? t[j + 1] << (64 - bs) : 0);
    }
    out[room - 1] = 0;  // the rounding carry's limb
    if (shift > 0) {
        const std::uint64_t hp   = shift - 1;  // the half bit
        const std::uint64_t hl   = hp / 64;
        const unsigned      hb   = static_cast<unsigned>(hp % 64);
        const bool          half = hl < n && ((t[hl] >> hb) & 1) != 0;
        if (half) {
            bool sticky = (t[hl] & ((Limb{1} << hb) - 1)) != 0;
            for (std::size_t i = 0; !sticky && i < hl; ++i) sticky = t[i] != 0;
            if (sticky || (out[0] & 1) != 0) increment(out, room);
        }
    }
    return significant(out, room);
}

// 10^scale, 2 x 10^scale and 4 x 10^scale, all `width` limbs: the three dividends of a term, built
// once before the window and read-only afterwards, shared by every worker of a run.
struct Context {
    std::size_t       width{0};
    std::vector<Limb> p1, p2, p4;
};

inline Context make_context(std::uint32_t precision) {
    Context c;
    c.p1 = {1};
    for (std::uint32_t left = precision; left > 0;) {
        const std::uint32_t step = std::min<std::uint32_t>(left, 9);
        std::uint32_t       m    = 1;
        for (std::uint32_t i = 0; i < step; ++i) m *= 10;
        mul_small(c.p1, m);
        left -= step;
    }
    c.p2 = c.p1;
    mul_small(c.p2, 2);
    c.p4 = c.p1;
    mul_small(c.p4, 4);
    c.width = c.p4.size();
    c.p1.resize(c.width, 0);
    c.p2.resize(c.width, 0);
    return c;
}

// A worker's scratch: two working integers of the context's width, kept across terms.
struct Scratch {
    std::vector<Limb> a, b;
};

// Savina's PiPrecisionConfig.calculateBbpTerm(scale, k), exactly: the unscaled value of
//
//     (4/(8k+1) - 2/(8k+4) - 1/(8k+5) - 1/(8k+6)) / 16^k
//
// each division at the scale, HALF_EVEN. The worker's kernel, shared by all four implementations,
// in three steps so that every adapter computes the term STRAIGHT INTO the buffer its message
// carries -- a heap buffer, a block, an allocated_push tail -- and none copies it once more:
//
//   bbp_numerator(c, k, s)       the four divisions and three subtractions, into the scratch; false
//                                if the difference went negative, which no term of this spec does
//                                (the reference asserts it for every term a run can hand out);
//   bbp_room(k, s)               the limbs the term can occupy;
//   bbp_shift(k, s, out)         the division by 16^k into `out` (bbp_room limbs), returning the
//                                term's significant limbs;
//   bbp_proof(s)                 the low limb of the numerator, which only the four long divisions
//                                produce -- the term's proof of work (THE VERIFIED ANSWER, below).
inline bool bbp_numerator(const Context &c, std::uint32_t k, Scratch &s) {
    const std::uint32_t e = 8 * k;
    s.a.resize(c.width);
    s.b.resize(c.width);
    div_round(c.p4.data(), c.width, e + 1, s.a.data());
    div_round(c.p2.data(), c.width, e + 4, s.b.data());
    bool negative = sub_in_place(s.a.data(), s.b.data(), c.width);
    div_round(c.p1.data(), c.width, e + 5, s.b.data());
    negative = sub_in_place(s.a.data(), s.b.data(), c.width) || negative;
    div_round(c.p1.data(), c.width, e + 6, s.b.data());
    negative = sub_in_place(s.a.data(), s.b.data(), c.width) || negative;
    return !negative;
}

inline std::size_t bbp_room(std::uint32_t k, const Scratch &s) noexcept {
    return shr_room(significant(s.a.data(), s.a.size()), 4ULL * k);
}

inline std::size_t bbp_shift(std::uint32_t k, const Scratch &s, Limb *out) noexcept {
    return shr_round(s.a.data(), significant(s.a.data(), s.a.size()), 4ULL * k, out);
}

inline Limb bbp_proof(const Scratch &s) noexcept { return s.a.empty() ? 0 : s.a[0]; }

// The three steps into a vector, sized to the term: what the form=0 adapters move into a message.
inline bool bbp_term(const Context &c, std::uint32_t k, Scratch &s, std::vector<Limb> &out) {
    const bool ok = bbp_numerator(c, k, s);
    out.resize(bbp_room(k, s));
    out.resize(bbp_shift(k, s, out.data()));
    return ok;
}

// Savina's stop test, `rm.result.compareTo(tolerance) <= 0` with tolerance 10^-scale: an unscaled
// value of at most 1.
inline bool at_tolerance(const Limb *v, std::size_t n) noexcept {
    return n == 0 || (n == 1 && v[0] <= 1);
}

// How many terms a run may hand out: every term of a correct run has an index below this. The
// first term at the tolerance, k1, has 16^k1 >= ~pi x 10^precision, so k1 <= 0.8305 x precision + 1
// (log16(10) = 0.83048...); the reference asserts its own k1 is below kTermsCap(). And the master
// keeps at most `batch` terms in flight, all handed out in index order, and stops handing out at the
// first answer at the tolerance: every index it hands out is at most k1 + batch.
inline std::uint32_t terms_cap(const Config &c) noexcept {
    return (c.precision * 831 + 999) / 1000 + 8 + c.batch;
}

// ---------------------------------------------------------------------------------------------
// The master's state and THE VERIFIED ANSWER
//
// Fold is what Savina's master keeps -- the sum and the count of terms handed out and answered --
// plus one count per term index, written by the master's handlers alone (one actor, one writer).
// next() hands out the next index, add() takes an answer in. Whatever order the answers arrive in,
// the sum is the integer sum of the terms handed out, and every term past the first one at the
// tolerance rounds to 0 at the scale (the reference asserts it for every index a run can reach):
// a correct run's sum is EXACTLY sum(term(k), k = 0..k1), the same integer for every interleaving
// and every framework, whatever number of terms past k1 the race handed out.
//
// Every answer carries, beside the term, its PROOF OF WORK: the low limb of the term's numerator --
// 4/(8k+1) - 2/(8k+4) - 1/(8k+5) - 1/(8k+6) at the scale, before the division by 16^k -- which only
// the four long divisions produce, and which the master records per index. A term past k1 is 0
// whether it was computed or not; its proof is not.
//
// What is compared, after the window: the sum, limb by limb; the number of term indices whose count
// is not what the protocol fixes (1 for every index handed out, 0 for every other); the number of
// indices handed out whose proof is not the reference's; the work messages the workers counted,
// against the terms handed out; and the answers received, against them too. So an answer delivered
// twice, or a work message delivered twice (two answers to one index), moves the anomaly count and,
// below k1, the sum; a worker that answers without computing, or computes another term, moves the
// proofs and, below k1, the sum -- past k1, the proofs alone; an answer or a work message LOST never
// ends the run -- the master waits for every term it handed out -- and the harness's deadline fails
// it (FAIRNESS.md section 0). The number of terms handed out depends on the interleaving: it is
// reported as the observation `terms`, never compared silently, and asserted to be at least k1 + 1
// -- the terms the answer needs (Spec::observed_at_least).
// ---------------------------------------------------------------------------------------------

struct Fold {
    std::vector<Limb>          sum;     // the context's width + 1 limbs: below 4 x 10^scale
    std::vector<std::uint32_t> got;     // answers received per term index
    std::vector<Limb>          proofs;  // the proof each index's answer carried
    std::uint32_t              requested{0};
    std::uint32_t              received{0};

    Fold(const Config &c, const Context &ctx)
        : sum(ctx.width + 1, 0), got(terms_cap(c), 0), proofs(terms_cap(c), 0) {}

    // The index of the next term to hand out. More than the protocol can hand out is a protocol
    // violation no checksum could report in time: qvo::fail(), exit 1 (harness.h).
    std::uint32_t next() {
        if (requested == got.size())
            qvo::fail("savina/piprecision: protocol violated -- more terms handed out than a run "
                      "can reach (" + std::to_string(got.size()) + ")");
        return requested++;
    }

    // An answer: counted against its index, its proof recorded, added into the sum. Returns
    // Savina's stop test.
    bool add(std::uint32_t k, const Limb *v, std::size_t n, Limb proof) {
        if (k >= requested)
            qvo::fail("savina/piprecision: protocol violated -- an answer for term "
                      + std::to_string(k) + ", which was never handed out");
        if (n >= sum.size())
            qvo::fail("savina/piprecision: protocol violated -- an answer of " + std::to_string(n)
                      + " limbs, wider than any term at this precision");
        ++got[k];
        proofs[k] = proof;
        ++received;
        Limb carry = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Limb x = sum[i] + carry;
            carry        = static_cast<Limb>(x < carry);
            sum[i]       = x + v[i];
            carry += static_cast<Limb>(sum[i] < v[i]);
        }
        for (std::size_t i = n; carry != 0 && i < sum.size(); ++i) {
            sum[i] += carry;
            carry = static_cast<Limb>(sum[i] == 0);
        }
        return at_tolerance(v, n);
    }

    bool complete() const noexcept { return received == requested; }
};

// Each worker's count, a 128-byte slot of its own: no two workers, whatever thread runs them, write
// one line or one prefetched pair of lines (thread_ring::Tally says why).
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally {
    std::uint64_t works{0};  // the work messages this worker's handler ran
};

inline std::uint64_t works(const std::vector<Tally> &tallies) noexcept {
    std::uint64_t n = 0;
    for (const Tally &t : tallies) n += t.works;
    return n;
}

inline constexpr std::uint64_t kSumSalt       = 0x7069707265630001ULL;  // "piprec"
inline constexpr std::uint64_t kAnomalySalt   = 0x7069707265630002ULL;
inline constexpr std::uint64_t kWorksSalt     = 0x7069707265630003ULL;
inline constexpr std::uint64_t kReceivedSalt  = 0x7069707265630004ULL;
inline constexpr std::uint64_t kProofSalt     = 0x7069707265630005ULL;

inline std::uint64_t digest_sum(const std::vector<Limb> &sum) noexcept {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < sum.size(); ++i) acc += qvo::mix(qvo::mix(kSumSalt + i) ^ sum[i]);
    return acc;
}

// What every adapter returns, computed after teardown from the master's Fold, the workers' tallies
// and the reference's proofs (reference(p).proofs: built once per process, before the first
// repetition).
inline std::uint64_t checksum(const Fold &f, std::uint64_t worked,
                              const std::vector<Limb> &want) noexcept {
    std::uint64_t anomalies = 0, unproved = 0;
    for (std::size_t k = 0; k < f.got.size(); ++k) {
        anomalies += f.got[k] != (k < f.requested ? 1u : 0u);
        unproved += k < f.requested && (k >= want.size() || f.proofs[k] != want[k]);
    }
    return digest_sum(f.sum) + qvo::mix(kAnomalySalt ^ anomalies) + qvo::mix(kProofSalt ^ unproved)
           + qvo::mix(kWorksSalt ^ (worked - f.requested))
           + qvo::mix(kReceivedSalt ^ (static_cast<std::uint64_t>(f.received) - f.requested));
}

// The observation every adapter reports: the terms the master handed out.
inline constexpr const char *kObservedTerms = "terms";

// ---------------------------------------------------------------------------------------------
// The reference: the terms in index order, the first one at the tolerance found, every term a run
// can hand out past it checked to be 0, every term's proof, the exact sum. One run's work at one
// core (and a batch more), once per (precision, workers) and per process.
// ---------------------------------------------------------------------------------------------

struct Reference {
    std::uint32_t     k1{0};      // the first term at the tolerance
    std::uint64_t     answer{0};  // checksum() of a correct run
    std::vector<Limb> proofs;     // every term's proof, for every index a run can hand out
};

inline const Reference &reference(const qvo::Params &p) {
    static std::map<std::pair<long long, long long>, Reference> cache;  // the main thread's only
    const Config c   = config(p);
    const auto   key = std::make_pair(static_cast<long long>(c.precision),
                                      static_cast<long long>(c.workers));
    if (const auto it = cache.find(key); it != cache.end()) return it->second;

    const Context     ctx = make_context(c.precision);
    Scratch           s;
    std::vector<Limb> term;
    Fold              f(c, ctx);
    Reference         r;
    bool              found = false;
    r.proofs.assign(f.got.size(), 0);
    for (std::uint32_t k = 0; k < f.got.size(); ++k) {
        if (!bbp_term(ctx, k, s, term))
            qvo::refuse("savina/piprecision: term " + std::to_string(k)
                        + " went negative -- the spec's arithmetic is wrong");
        r.proofs[k] = bbp_proof(s);
        if (found) {
            if (!term.empty())
                qvo::refuse("savina/piprecision: term " + std::to_string(k) + " past k1="
                            + std::to_string(r.k1) + " is not 0 at the scale -- the stop "
                            "would make the sum depend on the interleaving");
            continue;
        }
        f.add(f.next(), term.data(), term.size(), r.proofs[k]);
        if (at_tolerance(term.data(), term.size())) {
            found = true;
            r.k1  = k;
        }
    }
    if (!found || r.k1 + c.batch >= f.got.size())
        qvo::refuse("savina/piprecision: the first term at the tolerance is past the spec's cap ("
                    + std::to_string(f.got.size()) + ") -- terms_cap() is wrong");
    r.answer = checksum(f, f.requested, r.proofs);
    return cache.emplace(key, r).first->second;
}

inline std::uint64_t expected(const qvo::Params &p) { return reference(p).answer; }

// The report divides by this: one term of the answer -- k1 + 1 of them, the terms the answer needs.
// A run hands out a few more (the race past k1, reported as `terms`), each one more work message,
// answer and division.
inline constexpr const char *kWorkUnit = "term";
inline std::uint64_t work_units(const qvo::Params &p) { return reference(p).k1 + 1; }

// The terms a run must hand out, asserted (Spec::observed_at_least): every one up to k1.
inline std::uint64_t min_terms(const qvo::Params &p) { return reference(p).k1 + 1; }

// NO expected_messages. The count is 2 x terms -- one work message and one answer per term handed
// out -- and how many terms the master hands out past k1 depends on the interleaving. Each of them
// is checked index by index instead (checksum() above), and `terms` is reported beside the cell.

}  // namespace qvospec::savina::piprecision

#endif  // QVOSPEC_SAVINA_PIPRECISION_H
