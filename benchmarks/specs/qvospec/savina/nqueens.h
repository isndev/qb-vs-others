// savina/nqueens — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: N-Queens first K solutions, `nqueenk` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- NQueensConfig.java for the parameters, NQueensAkkaActorBenchmark.scala
// for the semantics. Deviations are recorded in benchmarks/savina/nqueens.md.

#ifndef QVOSPEC_SAVINA_NQUEENS_H
#define QVOSPEC_SAVINA_NQUEENS_H

#include <qvo/harness.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace qvospec::savina::nqueens {

inline constexpr const char *kId = "savina/nqueens";

// The shape: a MASTER and a fixed pool of `workers` WORKERS, all created before the window --
// nothing is created inside it. The master hands one work item to a worker, round-robin over the
// pool; a work item is a partial board (the column of the queen in each of the first `depth`
// rows). A worker that receives an item:
//
//   * depth == size       -> the board is a solution: one RESULT to the master;
//   * depth >= threshold  -> searches the rest of the tree SEQUENTIALLY, one RESULT per solution;
//   * otherwise           -> extends the board by one row in every column that keeps it valid and
//                            sends each extension to the MASTER as a new work item, which the
//                            master forwards to the next worker in its rotation;
//
// and then sends the master one DONE. The master stops when it has received as many DONE as it
// has forwarded items. That termination is sound because every channel is per-sender FIFO: a
// worker's child items and results reach the master before the DONE of the item that made them.
//
// It is the suite's work-distribution shape: almost all the time is the search itself (the
// kernel below, identical in every implementation), and what a framework decides is WHERE that
// search runs -- a statically placed worker pool, a work-stealing pool, or a thread pool -- and
// what a relay through one master costs (4 959 items and 14 200 results at the defaults).
//
// `size`      -- the board. Savina's own default, 12 (14 200 solutions).
// `threshold` -- the depth from which a worker stops splitting and searches sequentially.
//                Savina's own default, 4.
// `workers`   -- the pool. Savina's own default, 20; worker w on core w % cores where the
//                framework places.
// `cores`     -- worker-thread budget. `wait` -- 1 = spin, 0 = park.
//
// NOT implemented, and why (nqueens.md has the detail): Savina's SOLUTIONS_LIMIT early stop --
// the master asks the workers to stop once it has counted that many results. At Savina's own
// defaults the limit (1 500 000) is above the number of 12-queens solutions (14 200), so the stop
// never fires and the whole tree is searched: the work measured here is the work Savina's
// defaults measure. Below the solution count the stop would make WHICH solutions were counted a
// race, and a race cannot be verified. Savina's PRIORITIES (the priority-mailbox variant) are not
// implemented either: the plain actor variant every framework runs ignores them.
inline std::map<std::string, long long> params() {
    return {{"size", 12}, {"threshold", 4}, {"workers", 20}, {"cores", 2}, {"wait", 1}};
}

inline constexpr int kMaxSize = 20;  // Savina's own cap (NQueensConfig.SOLUTIONS has 20 rows)

// Savina's NQueensConfig.SOLUTIONS: the number of solutions for size 1..20. expected() asserts
// its own enumeration against this table, so the reference cannot silently miscount.
inline constexpr std::uint64_t kSolutions[kMaxSize] = {
    1,       0,        0,        2,         10,        4,          40,
    92,      352,      724,      2680,      14200,     73712,      365596,
    2279184, 14772512, 95815104, 666090624, 4968057848ULL, 39029188884ULL};

// A partial board, packed into two 64-bit words so that it travels as one event in every
// framework and as the two payload words of the floor's message: the column (< 32, 5 bits) of
// row r's queen at bits 5r of w0 for r < 12 and 5(r - 12) of w1 for r >= 12, the depth in the
// top byte of w1.
struct Board {
    std::uint64_t w0{0};
    std::uint64_t w1{0};

    int depth() const noexcept { return static_cast<int>(w1 >> 56); }

    void unpack(std::uint8_t *a) const noexcept {
        const int d = depth();
        for (int r = 0; r < d; ++r)
            a[r] = static_cast<std::uint8_t>(
                (r < 12 ? w0 >> (5 * r) : w1 >> (5 * (r - 12))) & 31u);
    }

    static Board pack(const std::uint8_t *a, int depth) noexcept {
        Board b;
        for (int r = 0; r < depth; ++r) {
            if (r < 12) b.w0 |= static_cast<std::uint64_t>(a[r]) << (5 * r);
            else b.w1 |= static_cast<std::uint64_t>(a[r]) << (5 * (r - 12));
        }
        b.w1 |= static_cast<std::uint64_t>(depth) << 56;
        return b;
    }
};

// The identity of a board: a fold of its columns, row by row. Distinct boards fold to distinct
// values (up to a 64-bit collision), and a board of depth d and its parent fold differently.
inline constexpr std::uint64_t kHashSeed = 0x6a09e667f3bcc909ULL;
inline std::uint64_t board_hash(const std::uint8_t *a, int depth) noexcept {
    std::uint64_t h = kHashSeed;
    for (int r = 0; r < depth; ++r) h = qvo::mix(h + a[r] + 1);
    return h;
}

// What a DONE carries: the identity of the item it completes, salted so that it never equals the
// RESULT of the same board (a board of depth == size is both an item and a solution).
inline constexpr std::uint64_t kDoneSalt = 0xbb67ae8584caa73bULL;
inline std::uint64_t done_value(const Board &item) noexcept {
    std::uint8_t a[kMaxSize];
    item.unpack(a);
    return qvo::mix(board_hash(a, item.depth()) + kDoneSalt);
}

// ---------------------------------------------------------------------------------------------
// The worker's kernel -- Savina's nqueensKernelPar / nqueensKernelSeq, shared by all four
// implementations so that every framework does exactly the same search per item. Like Savina's,
// it re-validates the WHOLE board (every pair of queens, boardValid) for every candidate column:
// that O(depth^2) check is the work Savina specifies per node, not an optimisation left undone.
// It allocates nothing (Savina allocates a fresh array per candidate; the semantics are the same).
// ---------------------------------------------------------------------------------------------

// Savina's NQueensConfig.boardValid: no two of the first n queens share a column or a diagonal.
inline bool board_valid(int n, const std::uint8_t *a) noexcept {
    for (int i = 0; i < n; ++i) {
        const int p = a[i];
        for (int j = i + 1; j < n; ++j) {
            const int q = a[j];
            if (q == p || q == p - (j - i) || q == p + (j - i)) return false;
        }
    }
    return true;
}

template <typename OnResult>
void search_sequential(std::uint8_t *a, int depth, int size, OnResult &on_result) {
    if (depth == size) {
        on_result(board_hash(a, size));
        return;
    }
    for (int c = 0; c < size; ++c) {
        a[depth] = static_cast<std::uint8_t>(c);
        if (board_valid(depth + 1, a)) search_sequential(a, depth + 1, size, on_result);
    }
}

// Processes one work item. `on_work(Board)` is called once per child item (depth < threshold),
// `on_result(uint64 board_hash)` once per solution found; the caller sends the DONE afterwards.
template <typename OnWork, typename OnResult>
void process(const Board &item, int size, int threshold, OnWork &&on_work, OnResult &&on_result) {
    std::uint8_t a[kMaxSize];
    item.unpack(a);
    const int depth = item.depth();
    if (depth == size) {
        on_result(board_hash(a, size));
    } else if (depth >= threshold) {
        search_sequential(a, depth, size, on_result);
    } else {
        for (int c = 0; c < size; ++c) {
            a[depth] = static_cast<std::uint8_t>(c);
            if (board_valid(depth + 1, a)) on_work(Board::pack(a, depth + 1));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The reference: an INDEPENDENT enumeration -- column and diagonal bitmasks, the hash folded
// incrementally -- that shares no search code with the kernel above. A kernel that missed or
// invented a solution would disagree with it, not agree by construction.
// ---------------------------------------------------------------------------------------------

struct Tally {
    std::uint64_t items{0};      // work items: every valid board of depth <= threshold
    std::uint64_t solutions{0};  // every valid board of depth == size
    std::uint64_t checksum{0};
};

// The two diagonal masks index row + col (0..38) and row - col + kMaxSize (1..39): 64-bit.
inline void enumerate(int depth, int size, int threshold, std::uint64_t cols, std::uint64_t d1,
                      std::uint64_t d2, std::uint64_t h, Tally &t) {
    if (depth <= threshold) {
        ++t.items;
        t.checksum += qvo::mix(h + kDoneSalt);
    }
    if (depth == size) {
        ++t.solutions;
        t.checksum += h;
        return;
    }
    for (int c = 0; c < size; ++c) {
        const std::uint64_t bit = std::uint64_t{1} << c;
        const std::uint64_t a   = std::uint64_t{1} << (depth + c);
        const std::uint64_t b   = std::uint64_t{1} << (depth - c + kMaxSize);
        if ((cols & bit) || (d1 & a) || (d2 & b)) continue;
        enumerate(depth + 1, size, threshold, cols | bit, d1 | a, d2 | b,
                  qvo::mix(h + static_cast<std::uint64_t>(c) + 1), t);
    }
}

inline Tally reference(const qvo::Params &p) {
    const long long size      = p.get("size");
    const long long threshold = p.get("threshold");
    const long long workers   = p.get("workers");
    if (size < 1 || size > kMaxSize || threshold < 1 || workers < 1) {
        std::fprintf(stderr, "savina/nqueens: need 1 <= size <= %d, threshold >= 1, workers >= 1 "
                             "(got size=%lld threshold=%lld workers=%lld)\n",
                     kMaxSize, size, threshold, workers);
        std::abort();
    }
    Tally t;
    enumerate(0, static_cast<int>(size), static_cast<int>(threshold), 0, 0, 0, kHashSeed, t);
    if (t.solutions != kSolutions[size - 1]) {
        std::fprintf(stderr, "savina/nqueens: the reference counted %llu solutions for size %lld, "
                             "Savina's table says %llu\n",
                     static_cast<unsigned long long>(t.solutions), size,
                     static_cast<unsigned long long>(kSolutions[size - 1]));
        std::abort();
    }
    return t;
}

// The checksum: the wrapping sum of every RESULT (the solution's board_hash) and every DONE
// (done_value of the item). A work item lost, duplicated or delivered twice changes the set of
// boards searched and therefore both sums; a result lost or duplicated changes the first, a done
// the second (and a lost done never terminates). A worker that answered without searching would
// have to produce every solution's hash, which is the search.
inline std::uint64_t expected(const qvo::Params &p) { return reference(p).checksum; }

// The report divides by this: one solution found and reported to the master.
inline constexpr const char *kWorkUnit = "solution";
inline std::uint64_t work_units(const qvo::Params &p) { return reference(p).solutions; }

// Counted at the receivers, inside the window: each worker counts the item it received and says
// so in that item's DONE (items); the master counts every child item it receives to forward
// (items - 1: all but the first, which it made itself), every DONE (items) and every RESULT
// (solutions). The ready handshake before the window and the shutdown after it are not counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Tally t = reference(p);
    return 3 * t.items - 1 + t.solutions;
}

}  // namespace qvospec::savina::nqueens

#endif  // QVOSPEC_SAVINA_NQUEENS_H
