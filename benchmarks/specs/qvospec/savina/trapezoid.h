// savina/trapezoid — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0).
//
// Savina reference: Trapezoidal Approximation, `trapezoid` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- TrapezoidalConfig.java for the parameters and the integrand,
// TrapezoidalAkkaActorBenchmark.scala for the semantics. Deviations are recorded in
// benchmarks/savina/trapezoid.md.

#ifndef QVOSPEC_SAVINA_TRAPEZOID_H
#define QVOSPEC_SAVINA_TRAPEZOID_H

#include <qvo/harness.h>

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// The one compiled body of the integration kernel (integrate() below): a call, never inlined, so
// the reference and every worker of a binary run the SAME machine code -- the condition for a
// floating-point answer to be compared bit for bit (see "THE VERIFIED ANSWER").
#if defined(_MSC_VER)
#define QVOSPEC_TRAPEZOID_NOINLINE __declspec(noinline)
#else
#define QVOSPEC_TRAPEZOID_NOINLINE __attribute__((noinline))
#endif

namespace qvospec::savina::trapezoid {

inline constexpr const char *kId = "savina/trapezoid";

// The shape, Savina's own: a MASTER and `workers` WORKERS, all created before the window. The
// master splits [left, right] into `workers` contiguous ranges of the `pieces` trapezoids and
// sends each worker ONE work message; a worker integrates its range -- two evaluations of Savina's
// integrand per trapezoid -- and answers ONE result naming itself; the master folds the results
// and the run ends at the last one. A fan-out and a fan-in of pure computation: 2 x `workers`
// messages against `2 x pieces` evaluations of a transcendental function, so the cell is the
// computation and what a framework decides is WHERE it runs -- and whether the second core gets
// half of it.
//
// `pieces`  -- trapezoids. Savina's own default, 10 000 000 (N).
// `workers` -- Savina's own default, 100 (W); worker w on core w % cores where the framework places.
// `left`, `right` -- the interval. Savina's own defaults, 1 and 5 (L, R); integers here because
//              every qvo parameter is (harness.h), which Savina's two defaults are.
// `cores`   -- worker-thread budget. `wait` -- 1 = spin, 0 = park.
inline std::map<std::string, long long> params() {
    return {{"pieces", 10000000}, {"workers", 100}, {"left", 1},
            {"right", 5},         {"cores", 2},     {"wait", 1}};
}

// The parameters, checked by config() before anything is built: at least one trapezoid per worker
// (Savina's worker with an empty range would answer 0 for nothing), counts small enough for every
// piece index to be an exact double, and an interval that is one. Outside that, the run is refused
// with the harness's fatal exit 2 (qvo::refuse).
inline constexpr long long kMaxPieces  = 1LL << 40;
inline constexpr long long kMaxWorkers = 1LL << 20;
inline constexpr long long kMaxBound   = 1LL << 20;
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53
              && std::numeric_limits<double>::max_exponent == 1024,
              "trapezoid's finite arithmetic bound requires IEEE binary64");

// Coordinate arithmetic is positive and normal: the integer inputs and every i < 2^40 are
// exact doubles. With epsilon=2^-52, the rounded h, i*h, +left and +h give x <= R*(1+epsilon)^4.
// R < 2^20 therefore gives x-R < 2^-29 (the leading term is 2^-30). This slack covers separate
// operations and a contracted multiply-add; it is outside the integration kernel.
inline constexpr double kEndpointSlack = 0x1p-29;

struct Config {
    std::uint64_t pieces{0};
    std::uint32_t workers{0};
    double        left{0};
    double        h{0};  // the width of one trapezoid, Savina's `precision`: (R - L) / N
};

inline Config config(const qvo::Params &p) {
    const long long pieces  = p.get("pieces");
    const long long workers = p.get("workers");
    const long long left    = p.get("left");
    const long long right   = p.get("right");
    if (workers < 1 || workers >= kMaxWorkers || pieces < workers || pieces >= kMaxPieces
        || left < 0 || right <= left || right >= kMaxBound)
        qvo::refuse("savina/trapezoid: need 1 <= workers < 2^20, workers <= pieces < 2^40 and "
                    "0 <= left < right < 2^20 (got pieces=" + std::to_string(pieces) + " workers="
                    + std::to_string(workers) + " left=" + std::to_string(left) + " right="
                    + std::to_string(right) + ")");
    // Directed downward rounding can turn exp overflow into a finite saturated value. This
    // benchmark's bitwise reference and finite-envelope admission require the default mode.
    if (std::fegetround() != FE_TONEAREST)
        qvo::refuse("savina/trapezoid: finite arithmetic requires round-to-nearest mode");
    Config c;
    c.pieces  = static_cast<std::uint64_t>(pieces);
    c.workers = static_cast<std::uint32_t>(workers);
    c.left    = static_cast<double>(left);
    c.h       = static_cast<double>(right - left) / static_cast<double>(pieces);
    // The integrand's magnitude envelope is monotone in x after dropping |sin|/(x+1) <= 1.
    // Round the coordinate/root envelope outward, then evaluate it once before reference work,
    // framework setup or a measured window. The binary64/round-to-nearest, no-fast-math
    // contract keeps isfinite meaningful and exp's overflow result observable here.
    const double infinity = std::numeric_limits<double>::infinity();
    const double x_upper = std::nextafter(static_cast<double>(right) + kEndpointSlack, infinity);
    const double argument_upper = std::nextafter(std::sqrt(2.0 * x_upper), infinity);
    const double magnitude_upper = std::sqrt(1.0 + std::exp(argument_upper));
    if (!std::isfinite(magnitude_upper))
        qvo::refuse("savina/trapezoid: interval exceeds the finite binary64 integrand envelope "
                    "(got left=" + std::to_string(left) + " right=" + std::to_string(right) + ")");
    // Even the loose |fx| <= 2^513 bound leaves the endpoint sum <= 2^514, one h-weighted term
    // <= 2^533, any <2^40-term absolute prefix <2^574 and a <2^20-worker fold <2^595.
    // Accumulation rounding contributes less than a factor two at either count. Cancellation
    // cannot invalidate those absolute bounds, all far below binary64's finite exponent limit.
    return c;
}

// Worker w's range of trapezoids, [first_piece(w), first_piece(w + 1)): the integer partition of
// the `pieces`, every worker within one trapezoid of N / W. A DEVIATION in rounding only
// (trapezoid.md): Savina sends each worker its interval as two doubles and the worker recovers its
// count as `((r - l) / h).toInt`, a truncation that can drop a trapezoid at a range's edge.
inline std::uint64_t first_piece(std::uint64_t w, const Config &c) noexcept {
    return w * c.pieces / c.workers;
}

// Savina's integrand, TrapezoidalConfig.fx, operation for operation.
inline double fx(double x) noexcept {
    const double a = std::sin(std::pow(x, 3.0) - 1.0);
    const double b = x + 1.0;
    const double c = a / b;
    const double d = std::sqrt(1.0 + std::exp(std::sqrt(2.0 * x)));
    return c * d;
}

// The worker's kernel -- Savina's Worker.process, shared by all four implementations so that every
// framework does exactly the same arithmetic per trapezoid: the left end of trapezoid i, the right
// end one width further, both evaluated (Savina evaluates every shared end twice, and so does
// this), the area added in order. A trapezoid's left end is `i * h + left` over the GLOBAL index i
// -- Savina's is `i * h + l` over the worker's own index from the worker's left end, which moves
// the last bits of x with the partition, not the work.
QVOSPEC_TRAPEZOID_NOINLINE inline double integrate(std::uint64_t begin, std::uint64_t end,
                                                    double left, double h) noexcept {
    double area = 0.0;
    for (std::uint64_t i = begin; i < end; ++i) {
        const double lx = static_cast<double>(i) * h + left;
        const double rx = lx + h;
        area += 0.5 * (fx(lx) + fx(rx)) * h;
    }
    return area;
}

// ---------------------------------------------------------------------------------------------
// THE VERIFIED ANSWER
//
// The answer is a double, and Savina's master folds the results in whatever order they arrive:
// floating-point addition is not associative, so its area moves in the last bits from one run to
// the next. Here nothing is folded in arrival order. The master keeps one Record per worker (the
// results naming it, the sum of their areas -- one result, one area, in a correct run), written by
// the master's handler alone, and once the last result is in it folds the records in WORKER order
// (fold() below, `workers` additions inside the window, Savina's own reduction moved to the end).
// Each worker's area is integrate() over a range fixed by the parameters, in a fixed order, through
// the one compiled body of the binary: a framework that ran the work answers EXACTLY the bits the
// reference computes, every interleaving alike, and the comparison is bitwise.
//
// What is compared, after the window: for every worker, its index, the results the master received
// naming it, the bits of their summed area and the work messages the worker itself received (its
// Tally, a 128-byte slot of its own: no two workers, whatever thread runs them, write one line or
// one prefetched pair of lines -- thread_ring::Tally says why), digested together; and the bits of
// the fold. So a work message delivered twice (two results, twice the area), a result delivered
// twice, a work message delivered to the wrong worker (one worker's range under another's name, an
// empty record beside it), a worker that answers without integrating, or with another range,
// each moves the sum. A work message or a result LOST never ends the run: the master waits for
// `workers` results, and the harness's deadline fails it (FAIRNESS.md section 0).
// ---------------------------------------------------------------------------------------------

struct Record {
    std::uint64_t results{0};  // the results the master received naming this worker
    double        area{0.0};   // the sum of their areas
    void          add(double a) noexcept {
        ++results;
        area += a;
    }
};

inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally {
    std::uint64_t works{0};  // the work messages this worker's handler ran
};

// The master's fold, in worker order -- the answer, Savina's "Area".
inline double fold(const std::vector<Record> &records) noexcept {
    double total = 0.0;
    for (const Record &r : records) total += r.area;
    return total;
}

inline std::uint64_t bits(double v) noexcept { return std::bit_cast<std::uint64_t>(v); }

inline constexpr std::uint64_t kTotalSalt = 0x74726170657a6f69ULL;  // "trapezoi"

inline std::uint64_t digest(std::uint64_t worker, std::uint64_t results, std::uint64_t area_bits,
                            std::uint64_t works) noexcept {
    return qvo::mix(qvo::mix(qvo::mix((worker << 40) ^ results) ^ area_bits) ^ works);
}

// What every adapter returns, computed after teardown from the master's records, the fold it
// produced and the workers' tallies.
inline std::uint64_t checksum(const std::vector<Record> &records, double total,
                              const std::vector<Tally> &tallies) noexcept {
    std::uint64_t acc = qvo::mix(kTotalSalt ^ bits(total));
    for (std::size_t w = 0; w < records.size(); ++w)
        acc += digest(w, records[w].results, bits(records[w].area),
                      w < tallies.size() ? tallies[w].works : 0);
    return acc;
}

// Every delivery the handlers ran: the workers' work messages and the master's results.
inline std::uint64_t delivered(const std::vector<Record> &records,
                               const std::vector<Tally> &tallies) noexcept {
    std::uint64_t n = 0;
    for (const Record &r : records) n += r.results;
    for (const Tally &t : tallies) n += t.works;
    return n;
}

// The framework-free answer: the same integration, worker by worker, through the same compiled
// kernel, folded in the same order. It costs one repetition's work at one core, once per process,
// before the first repetition.
inline std::uint64_t expected(const qvo::Params &p) {
    const Config        c = config(p);
    std::vector<Record> records(c.workers);
    std::vector<Tally>  tallies(c.workers);
    for (std::uint32_t w = 0; w < c.workers; ++w) {
        const double area = integrate(first_piece(w, c), first_piece(w + 1, c), c.left, c.h);
        if (!std::isfinite(area))
            qvo::refuse("savina/trapezoid: nonfinite reference area for worker " + std::to_string(w));
        records[w].add(area);
        tallies[w].works = 1;
    }
    const double total = fold(records);
    if (!std::isfinite(total)) qvo::refuse("savina/trapezoid: nonfinite reference fold");
    return checksum(records, total, tallies);
}

// The report divides by this: one trapezoid, two evaluations of the integrand.
inline constexpr const char *kWorkUnit = "trapezoid";
inline std::uint64_t work_units(const qvo::Params &p) { return config(p).pieces; }

// One work message per worker and one result back -- COUNTED at the receivers (delivered() above),
// never declared. The ready handshake before the window and the shutdown after it are not counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    return 2 * static_cast<std::uint64_t>(config(p).workers);
}

}  // namespace qvospec::savina::trapezoid

#endif  // QVOSPEC_SAVINA_TRAPEZOID_H
