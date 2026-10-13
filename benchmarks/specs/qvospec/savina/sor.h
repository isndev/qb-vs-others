// savina/sor — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0). The rules the ping-pong spec
// states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Successive Over-Relaxation, `sor` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- SucOverRelaxConfig.java for the parameters and the input,
// SucOverRelaxAkkaActorBenchmark.scala for the semantics. Deviations are recorded in
// benchmarks/savina/sor.md.

#ifndef QVOSPEC_SAVINA_SOR_H
#define QVOSPEC_SAVINA_SOR_H

#include <qvo/harness.h>

#include <cstddef>
#include <cstdint>
#ifdef QVO_REAL_ADAPTER_CONTROLS
#include <cstdio>
#include <cstdlib>
#endif
#include <string>
#include <vector>

namespace qvospec::savina::sor {

inline constexpr const char *kId = "savina/sor";

// The shape, Savina's own: a `size` x `size` grid with ONE ACTOR PER CELL, coloured like a
// checkerboard, relaxing towards the solution of Laplace's equation with zero outside the grid.
// A cell's neighbours are its 2 to 4 orthogonal neighbours (Savina's SorActor.neighbors). The
// BLACK cells -- (row + column) even, Savina's colour 1 -- send their value to every neighbour on
// Start; from then on every cell, once it holds one value from each neighbour, relaxes
//
//     value = omega / 4 * (sum of the neighbours' values) + (1 - omega) * value
//
// and sends the new value to every neighbour. Neighbours always have the other colour, so the
// exchange is the red-black ordering of Gauss-Seidel: a red cell's r-th update reads its black
// neighbours' (r-1)-th values, a black cell's r-th update the red neighbours' r-th. A red cell
// makes `iterations` updates and a black one `iterations - 1` (Savina counts a black cell's first
// send as one of its iterations); each reports its final value to its OWNER and stops.
//
// The owners are Savina's: a RUNNER that owns the cells of the left `size / 2` columns and a PEER
// that owns the rest (SorRunner and SorPeer -- the reference splits the grid between two actors
// that each create, start and collect their half); the peer reports its half to the runner, which
// ends the run.
//
// It is the suite's stencil: the handlers are tiny -- one add per value, one multiply-add and up
// to four sends per update -- so the cell measures message passing between many small actors with
// a dependency on every neighbour, the opposite of savina/apsp's long handlers
// (benchmarks/savina/sor.md, "What it can and cannot show").
//
// `size`       -- the grid's edge, one of Savina's DATA_SIZES (20, 80, 100, ...; its `-n` picks
//                 one). DEVIATION FROM SAVINA: its default is n = 0, a 20 x 20 grid, ~150 000
//                 values per run -- 1.4 ms for qb at one core on the development host, the regime
//                 where thread-ring's and ping-pong's counts were raised (sor.md). The default here
//                 is Savina's next size, n = 1: 80 x 80, 6 400 cell actors, 2.5 M values.
//                 `--param size=20` reproduces Savina's default.
// `iterations` -- Savina's JACOBI_NUM_ITER, 100, a constant there and a parameter here.
// `cores`      -- the worker budget. Where a framework places, the runner's half on the first half
//                 of the cores and the peer's on the second: at cores=2 the two halves of the grid
//                 on two cores, so only the values across the middle column cross a core.
// `wait`       -- 1 = spin, 0 = park.
inline std::map<std::string, long long> params() {
    return {{"size", 80}, {"iterations", 100}, {"cores", 2}, {"wait", 1}};
}

inline constexpr std::uint32_t kMaxSize       = 1024;
inline constexpr std::uint32_t kMaxIterations = 100000;

struct Shape {
    std::uint32_t size{0};
    std::uint32_t part{0};        // the runner's columns: 0 .. part-1 (Savina's `part = s / 2`)
    std::uint32_t iterations{0};
    std::uint32_t cells{0};
    std::uint32_t cores{0};
};

// The parameters, checked before anything is built (qvo::refuse, exit 2): a grid with a column on
// each side of the split, an iteration count from 1 -- Savina's own blocks at 1, because its black
// cells only report from a value handler; here a black cell with no update to make reports on
// Start -- and every core holding at least one column where the framework places.
inline Shape shape(const qvo::Params &p) {
    const long long size       = p.get("size");
    const long long iterations = p.get("iterations");
    const long long cores      = p.get("cores");
    if (size < 2 || size > kMaxSize || iterations < 1 || iterations > kMaxIterations || cores < 1
        || cores > size)
        qvo::refuse("savina/sor: need 2 <= size <= " + std::to_string(kMaxSize)
                    + ", 1 <= iterations <= " + std::to_string(kMaxIterations)
                    + ", 1 <= cores <= size (got size=" + std::to_string(size) + " iterations="
                    + std::to_string(iterations) + " cores=" + std::to_string(cores) + ")");
    Shape s;
    s.size       = static_cast<std::uint32_t>(size);
    s.part       = s.size / 2;
    s.iterations = static_cast<std::uint32_t>(iterations);
    s.cells      = s.size * s.size;
    s.cores      = static_cast<std::uint32_t>(cores);
    return s;
}

// Cell (row, column) is index row * size + column; its owner is the runner below column `part`.
inline bool runner_owns(const Shape &s, std::uint32_t cell) noexcept { return cell % s.size < s.part; }

// Where a framework that places puts cell `cell`: the runner's columns spread over the first
// cores / 2 cores, the peer's over the rest (one core holds both at cores=1). The runner lives on
// core 0, the peer on the core of its first column.
inline std::uint32_t core_of(const Shape &s, std::uint32_t cell) noexcept {
    const std::uint32_t c  = cell % s.size;
    const std::uint32_t c0 = s.cores / 2;
    return c < s.part ? c * c0 / s.part : c0 + (c - s.part) * (s.cores - c0) / (s.size - s.part);
}
inline std::uint32_t runner_core(const Shape &) noexcept { return 0; }
inline std::uint32_t peer_core(const Shape &s) noexcept { return core_of(s, s.part); }

// ---------------------------------------------------------------------------------------------
// The exact formulation -- a DEVIATION from Savina's doubles (sor.md)
// ---------------------------------------------------------------------------------------------
//
// Savina relaxes doubles and validates the grand total against REF_VAL to 1e-12. Two things in
// its protocol make a double's answer depend on the scheduler: a cell adds its neighbours' values
// in the order they ARRIVE (`sum += message.v`), and each owner adds its cells' final values in the
// order THEY report -- floating-point addition is not associative, so two frameworks, or two runs
// of one, can disagree in the last bits, and a bit-exact checksum cannot be written. So the values
// here are FIXED-POINT integers and the relaxation is integer arithmetic:
//
//   * omega = 5/4 (Savina's 1.25, exactly), so omega/4 = 5/16 and 1 - omega = -4/16, and
//     value' = floor((5 * sum - 4 * value) / 16) -- an arithmetic shift, which C++20 defines on a
//     negative value as the floor;
//   * the sum is integer addition, the same whatever order the values arrive in, and is formed in
//     unsigned (wrapping) arithmetic, so no order and no defect can make it undefined;
//   * the initial value of cell (i, j) is Savina's own: its PseudoRandom(10101010) drawn row-major
//     over the grid, A[i][j] = 1e-6 / (nextLong() + 1), here floor(2^40 / (nextLong() + 1)) -- the
//     same numbers in units of 1e-6 / 2^40;
//   * the answer is reduced AFTER the window, cell by cell in index order, from what each cell
//     reported to its owner -- never in report order.
//
// The magnitudes stay far inside 64 bits: an update is at most 1.5 x the largest value it reads,
// the relaxation contracts, and reference() refuses a parameter set whose values ever reach 2^56.
inline constexpr std::uint64_t kSeed  = 10101010;  // SucOverRelaxConfig.RANDOM_SEED
inline constexpr int           kScale = 40;        // 1e-6 is 2^40 units

inline std::vector<std::int64_t> initial(const Shape &s) {
    std::vector<std::int64_t> v(s.cells);
    std::uint64_t             value = kSeed;
    for (std::uint32_t c = 0; c < s.cells; ++c) {
        value = (value * 1309 + 13849) & 65535;  // Savina's PseudoRandom.nextLong
        v[c]  = static_cast<std::int64_t>((std::uint64_t{1} << kScale) / (value + 1));
    }
    return v;
}

// One relaxation: `sum` is the neighbours' values added in wrapping unsigned arithmetic.
inline std::int64_t relax(std::uint64_t sum, std::int64_t value) noexcept {
    const std::uint64_t t = 5 * sum - 4 * static_cast<std::uint64_t>(value);
    return static_cast<std::int64_t>(t) >> 4;
}

// The WITNESS of a cell's updates: after each one, witness = witness * kWitness + the new value
// (wrapping). The final value alone does not prove the updates ran right: the relaxation contracts
// and the floor rounds, so an error made half-way through dies out before the end (the negative
// control's off-by-one plant: one value one unit off at one update, every final value unchanged)
// -- but the witness of the cell that made it, and of the cells it reached, is not the
// reference's. Order-sensitive, so two wrong updates cannot cancel. One multiply-add per update.
inline constexpr std::uint64_t kWitness = 0x100000001b3ULL;
inline std::uint64_t witness(std::uint64_t acc, std::int64_t value) noexcept {
    return acc * kWitness + static_cast<std::uint64_t>(value);
}

inline bool black(const Shape &s, std::uint32_t cell) noexcept {
    return ((cell / s.size) + (cell % s.size)) % 2 == 0;
}

// A protocol violation the checksum could not see in time -- a value delivered twice, from a cell
// that is not a neighbour, carrying a generation the cell cannot be waiting for, or to a cell that
// has finished -- stops here, through qvo::fail() (exit 1 and a document, from the thread that saw
// it; never std::abort -- harness.h).
[[noreturn]] inline void fail(const std::string &what) {
    qvo::fail("savina/sor: protocol violated -- " + what);
}

// ---------------------------------------------------------------------------------------------
// One cell, framework-free, as every adapter's actor drives it
// ---------------------------------------------------------------------------------------------
//
//   on Start:   if (start()) publish();               if (report_due()) report to the owner
//   on a value: if (accept(g, from, v)) { update(); if (publishes()) publish(); }
//               if (report_due()) report to the owner
//
// where publish() sends (generation(), index(), value()) to every neighbour in for_each_neighbour's
// order, and the report carries (index(), value(), updates(), received(), work()).
//
// A value carries the SENDER's generation -- its update count -- and its index: the exchange of
// two neighbours strictly alternates (a cell sends again only after its neighbour has answered),
// so a cell can only ever receive the generation it is waiting for, from each neighbour once.
// Savina's value message carries the value alone; the two words more are what lets a cell refuse a
// duplicate, a misrouted value or a value of the wrong round on arrival (sor.md).
class Cell {
public:
    Cell() = default;
    Cell(const Shape &s, std::uint32_t index, std::int64_t value) noexcept
        : size_(s.size)
        , iterations_(s.iterations)
        , index_(index)
        , black_(sor::black(s, index))
        , value_(value) {
        const std::uint32_t r = index / s.size;
        const std::uint32_t c = index % s.size;
        // Savina's SorActor.neighbors, in its order for each kind of cell: east, south, west, north
        // inside the grid; at a corner the vertical neighbour, then the horizontal one; on an edge
        // the neighbour inwards, then the two along the edge.
        auto add = [this](std::uint32_t n) { neighbour_[degree_++] = n; };
        const bool top = r == 0, bottom = r == s.size - 1, left = c == 0, right = c == s.size - 1;
        if (!top && !bottom && !left && !right) {
            add(index + 1);
            add(index + s.size);
            add(index - 1);
            add(index - s.size);
        } else if ((top || bottom) && (left || right)) {
            add(top ? index + s.size : index - s.size);
            add(left ? index + 1 : index - 1);
        } else if (top || bottom) {
            add(top ? index + s.size : index - s.size);
            add(index + 1);
            add(index - 1);
        } else {
            add(left ? index + 1 : index - 1);
            add(index + s.size);
            add(index - s.size);
        }
        for (std::uint32_t k = 0; k < degree_; ++k) full_ |= std::uint8_t(1u << k);
    }

    std::uint32_t index() const noexcept { return index_; }
    std::uint32_t degree() const noexcept { return degree_; }
    template <typename F>
    void for_each_neighbour(F &&f) const {
        for (std::uint32_t k = 0; k < degree_; ++k) f(neighbour_[k]);
    }

    // Start: true when the cell publishes now -- a black cell's generation 0.
    bool start() {
        if (started_) fail("cell " + std::to_string(index_) + " received a second Start");
        started_ = true;
        ++received_;
        return black_;
    }

    // A neighbour's value of generation `g`. True when the round is complete: the caller calls
    // update().
    bool accept(std::uint32_t g, std::uint32_t from, std::int64_t v) {
        if (finished())
            fail("cell " + std::to_string(index_) + " received a value from cell "
                 + std::to_string(from) + " after its last update");
        // A red cell's next update reads its black neighbours' generation `updates`, a black
        // cell's the red neighbours' generation `updates + 1`.
        const std::uint32_t want = black_ ? updates_ + 1 : updates_;
        if (g != want)
            fail("cell " + std::to_string(index_) + " waiting for generation "
                 + std::to_string(want) + " received generation " + std::to_string(g)
                 + " from cell " + std::to_string(from));
        std::uint32_t k = 0;
        while (k < degree_ && neighbour_[k] != from) ++k;
        if (k == degree_)
            fail("cell " + std::to_string(index_) + " received a value from cell "
                 + std::to_string(from) + ", which is not its neighbour");
        const std::uint8_t bit = std::uint8_t(1u << k);
        if (held_ & bit)
            fail("cell " + std::to_string(index_) + " received generation " + std::to_string(g)
                 + " from cell " + std::to_string(from) + " twice");
        held_ = std::uint8_t(held_ | bit);
        sum_ += static_cast<std::uint64_t>(v);
        ++received_;
        return held_ == full_;
    }

    void update() noexcept {
        value_ = relax(sum_, value_);
        work_  = witness(work_, value_);
        sum_   = 0;
        held_  = 0;
        ++updates_;
    }

    // After an update: a black cell publishes every generation (the red cells' next update reads
    // it); a red cell every generation but its last, which nobody reads -- Savina sends it to black
    // cells that have already exited (dead letters); here it is not sent (sor.md).
    bool publishes() const noexcept { return black_ || updates_ < iterations_; }
    bool finished() const noexcept { return updates_ == (black_ ? iterations_ - 1 : iterations_); }
    // A cell reports once it has finished AND received its Start: a red cell may finish before
    // its owner's Start reaches it, and the run must not end with a Start still in flight.
    bool report_due() noexcept {
        if (reported_ || !started_ || !finished()) return false;
        reported_ = true;
        return true;
    }

    std::uint32_t generation() const noexcept { return updates_; }
    std::int64_t  value() const noexcept { return value_; }
    std::uint32_t updates() const noexcept { return updates_; }
    std::uint64_t received() const noexcept { return received_; }
    std::uint64_t work() const noexcept { return work_; }

private:
    std::uint32_t size_{0};
    std::uint32_t iterations_{0};
    std::uint32_t index_{0};
    std::uint32_t neighbour_[4]{};
    std::uint32_t degree_{0};
    bool          black_{false};
    bool          started_{false};
    bool          reported_{false};
    std::uint8_t  held_{0};
    std::uint8_t  full_{0};
    std::int64_t  value_{0};
    std::uint64_t sum_{0};
    std::uint32_t updates_{0};
    std::uint64_t received_{0};
    std::uint64_t work_{0};
};

// ---------------------------------------------------------------------------------------------
// The owners' record, and the verified answer
// ---------------------------------------------------------------------------------------------

// What a cell reports, kept by its owner in the owner's own vector (one per owner, so the runner
// and the peer never write one allocation), read by the body once the framework is gone.
struct Report {
    std::int64_t  value{0};
    std::uint32_t updates{0};
    std::uint32_t filled{0};
    std::uint64_t received{0};
    std::uint64_t work{0};
};

// An owner's side of the protocol: it keeps the reports of ITS cells and refuses any other.
// Returns true when the owner holds every one of its cells' reports.
class Owner {
public:
    Owner() = default;
    Owner(const Shape &s, bool runner) : shape_(s), runner_(runner), reports_(s.cells) {
        for (std::uint32_t c = 0; c < s.cells; ++c)
            if (runner_owns(s, c) == runner) ++expected_;
    }
    bool record(std::uint32_t cell, std::int64_t value, std::uint32_t updates, std::uint64_t received,
                std::uint64_t work) {
        if (cell >= shape_.cells || runner_owns(shape_, cell) != runner_)
            fail(std::string(runner_ ? "the runner" : "the peer") + " received the report of cell "
                 + std::to_string(cell) + ", which it does not own");
        Report &r = reports_[cell];
        if (r.filled) fail("cell " + std::to_string(cell) + " reported twice");
        r = Report{value, updates, 1, received, work};
        ++received_;
        return ++held_ == expected_;
    }
    void count() noexcept { ++received_; }  // a message other than a report: Start, the peer's
    bool complete() const noexcept { return held_ == expected_; }
    std::uint64_t        received() const noexcept { return received_; }
    const Report        &report(std::uint32_t cell) const noexcept { return reports_[cell]; }

private:
    Shape               shape_{};
    bool                runner_{false};
    std::vector<Report> reports_;
    std::uint32_t       expected_{0};
    std::uint32_t       held_{0};
    std::uint64_t       received_{0};
};

// One cell's term: its index, its update count, the messages it received, the witness of its
// updates and its final value, mixed -- a cell that skipped an update, ran one twice, received a
// value too many or too few, or computed another value at any update moves the total; and a
// report filed under another cell's index moves it.
inline constexpr std::uint64_t kCellSalt = 0x5040302010000000ULL;
inline std::uint64_t digest(std::uint32_t cell, std::int64_t value, std::uint64_t updates,
                            std::uint64_t received, std::uint64_t work) noexcept {
    return qvo::mix(qvo::mix(qvo::mix(qvo::mix(qvo::mix(kCellSalt + cell) ^ updates) ^ received)
                             ^ work)
                    ^ static_cast<std::uint64_t>(value));
}

// What every adapter returns, computed after teardown from the two owners' reports, in cell index
// order: the checksum, and every delivery the handlers ran -- the cells' (their Start and their
// values, reported) and the owners' own.
inline std::uint64_t checksum(const Shape &s, const Owner &runner, const Owner &peer) noexcept {
    std::uint64_t acc = 0;
    for (std::uint32_t c = 0; c < s.cells; ++c) {
        const Report &r = runner_owns(s, c) ? runner.report(c) : peer.report(c);
        acc += digest(c, r.value, r.updates, r.received, r.work);
    }
    return acc;
}
inline std::uint64_t delivered(const Shape &s, const Owner &runner, const Owner &peer) noexcept {
    std::uint64_t n = runner.received() + peer.received();
    for (std::uint32_t c = 0; c < s.cells; ++c)
        n += (runner_owns(s, c) ? runner.report(c) : peer.report(c)).received;
    return n;
}

// Edges of the grid: every cell's degree, summed, is twice this.
inline std::uint64_t edges(const Shape &s) noexcept {
    return 2ull * s.size * (s.size - 1);
}

// The reference: the same red-black relaxation SEQUENTIALLY, sharing only relax(), witness() and
// the input with the cells -- the whole grid swept by colour, red then black, generation by
// generation, each cell's witness kept as it goes. It refuses a parameter set whose values ever
// reach 2^56 (none of the domain does; the check keeps the claim above honest).
struct Reference {
    std::vector<std::int64_t>  value;
    std::vector<std::uint64_t> witness;
};
inline Reference reference(const Shape &s) {
    Reference                  ref{initial(s), std::vector<std::uint64_t>(s.cells, 0)};
    std::vector<std::int64_t> &v = ref.value;
    const std::uint32_t       n = s.size;
    constexpr std::int64_t    kBound = std::int64_t{1} << 56;
    auto sweep = [&](bool colour_black) {
        for (std::uint32_t r = 0; r < n; ++r)
            for (std::uint32_t c = 0; c < n; ++c) {
                const std::uint32_t i = r * n + c;
                if (black(s, i) != colour_black) continue;
                std::uint64_t sum = 0;
                if (r > 0) sum += static_cast<std::uint64_t>(v[i - n]);
                if (r + 1 < n) sum += static_cast<std::uint64_t>(v[i + n]);
                if (c > 0) sum += static_cast<std::uint64_t>(v[i - 1]);
                if (c + 1 < n) sum += static_cast<std::uint64_t>(v[i + 1]);
                v[i]           = relax(sum, v[i]);
                ref.witness[i] = witness(ref.witness[i], v[i]);
                if (v[i] >= kBound || v[i] <= -kBound)
                    qvo::refuse("savina/sor: a value reached 2^56 in the reference -- outside the "
                                "domain the fixed-point formulation is exact in");
            }
    };
    for (std::uint32_t g = 1; g <= s.iterations; ++g) {
        sweep(false);                          // red update g reads black generation g - 1
        if (g < s.iterations) sweep(true);     // black update g reads red generation g
    }
    return ref;
}

// The framework-free answer: every cell with its reference value and witness, its closed-form
// update count (iterations for a red cell, iterations - 1 for a black one) and its received count
// -- its Start and one value from each neighbour per update.
inline std::uint64_t expected(const qvo::Params &p) {
    const Shape     s   = shape(p);
    const Reference r   = reference(s);
    std::uint64_t   acc = 0;
    for (std::uint32_t c = 0; c < s.cells; ++c) {
        const Cell          cell(s, c, 0);
        const std::uint64_t updates = black(s, c) ? s.iterations - 1 : s.iterations;
        acc += digest(c, r.value[c], updates, 1 + cell.degree() * updates, r.witness[c]);
    }
    return acc;
}

// The report divides by this: one relaxation of one cell -- its neighbours' values received, the
// update, its value sent on.
inline constexpr const char *kWorkUnit = "update";
inline std::uint64_t work_units(const qvo::Params &p) {
    const Shape         s     = shape(p);
    const std::uint64_t black = (static_cast<std::uint64_t>(s.cells) + 1) / 2;
    const std::uint64_t red   = s.cells - black;
    return red * s.iterations + black * (s.iterations - 1);
}

// COUNTED at the receivers, inside the window: a Start per cell and one to the peer, the values
// -- black generations 0 .. iterations-1 and red generations 1 .. iterations-1 cross every edge,
// edges * (2 * iterations - 1) -- a report per cell and the peer's report to the runner. The
// handshake before the window and the shutdown after it are not counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Shape s = shape(p);
    return 2ull * s.cells + 2 + edges(s) * (2ull * s.iterations - 1);
}


#ifdef QVO_REAL_ADAPTER_CONTROLS
// Compiled only into qvoctl-* built from the real adapters. Each cell owns its plant state.
class AdapterControl {
public:
    AdapterControl(const Shape &shape, std::uint32_t id)
        : shape_(shape), id_(id), mode_(control_mode()) {}

    bool accept(Cell &c, std::uint32_t g, std::uint32_t from, std::int64_t value) {
        const bool target = id_ == 7 % shape_.cells && c.updates() == shape_.iterations / 2 && !fired_;
        if (target && mode_ == "lost") {
            mark();
            return false;
        }
        if (target && mode_ == "misroute") {
            mark();
            from = id_;  // no cell is its own orthogonal neighbour
        }
        if (target && mode_ == "wrong-round") {
            mark();
            ++g;
        }
        const bool complete = c.accept(g, from, value);
        if (target && mode_ == "duplicate") {
            mark();
            c.accept(g, from, value);  // before update resets the receipt bits
        }
        return complete;
    }

    std::int64_t published_value(const Cell &c) {
        auto value = c.value();
        if (mode_ == "off-by-16" && !fired_ && id_ == 7 % shape_.cells
            && c.updates() == shape_.iterations / 2) {
            mark();
            // One raw transmitted value, sent to all this publication's neighbours alike.
            // +16 changes the receiver's 5/16 relaxation by exactly +5, even when its integer
            // floor would discard +1 entirely. Its actual update witness must see that error.
            value += 16;
        }
        return value;
    }

private:
    static std::string control_mode() {
        const char *v = std::getenv("QVO_CONTROL_PLANT");
        const std::string mode = v ? v : "none";
        if (mode != "none" && mode != "duplicate" && mode != "lost" && mode != "misroute"
            && mode != "wrong-round" && mode != "off-by-16")
            qvo::refuse("savina/sor adapter control: unknown plant " + mode);
        return mode;
    }
    void mark() {
        fired_ = true;
        std::fprintf(stderr, "control plant fired: %s\n", mode_.c_str());
        std::fflush(stderr);
    }
    Shape shape_;
    std::uint32_t id_;
    std::string mode_;
    bool fired_{false};
};

inline void explain_adapter_control(const Shape &s, const Owner &runner, const Owner &peer) {
    const Reference ref = reference(s);  // unmodified independent reference, after teardown
    bool counts = true, values = true;
    for (std::uint32_t c = 0; c < s.cells; ++c) {
        const Report &r = runner_owns(s, c) ? runner.report(c) : peer.report(c);
        const std::uint64_t updates = black(s, c) ? s.iterations - 1 : s.iterations;
        const Cell cell(s, c, 0);
        counts = counts && r.updates == updates && r.received == 1 + cell.degree() * updates;
        values = values && r.value == ref.value[c];
    }
    std::fprintf(stderr, "sor adapter control: every cell's updates and receipts as the closed form says: %s\n",
                 counts ? "yes" : "no");
    std::fprintf(stderr, "sor adapter control: every final value as the reference says: %s\n",
                 values ? "yes" : "no");
}
#endif

}  // namespace qvospec::savina::sor

#endif  // QVOSPEC_SAVINA_SOR_H
