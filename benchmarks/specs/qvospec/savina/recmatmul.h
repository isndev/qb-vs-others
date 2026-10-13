// savina/recmatmul — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0).
//
// Savina reference: Recursive Matrix Multiplication, `recmatmul` (Imam & Sarkar, AGERE 2014), of
// the "parallelism" group -- MatMulConfig.java for the parameters and the data,
// MatMulAkkaActorBenchmark.scala for the semantics. Deviations are recorded in
// benchmarks/savina/recmatmul.md.

#ifndef QVOSPEC_SAVINA_RECMATMUL_H
#define QVOSPEC_SAVINA_RECMATMUL_H

#include <qvo/harness.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

// The one compiled body of the leaf kernel (multiply() below): a call, never inlined, so every
// framework's binary runs it without the codegen of its own call site.
#if defined(_MSC_VER)
#define QVOSPEC_RECMATMUL_NOINLINE __declspec(noinline)
#else
#define QVOSPEC_RECMATMUL_NOINLINE __attribute__((noinline))
#endif

namespace qvospec::savina::recmatmul {

inline constexpr const char *kId = "savina/recmatmul";

// The shape, Savina's own: C = A x B for three `length` x `length` matrices, by recursive
// decomposition. A MASTER and `workers` WORKERS are created before the window. The master sends
// the whole product, as one TASK, to a worker; a worker that receives a task whose block count
// (dim^2) is above `threshold` SPLITS it -- eight tasks of half the dimension, the two halves of
// the inner index for each quarter of C, sent back to the MASTER -- and otherwise MULTIPLIES it,
// a dim^3 block product added into its quarter of C; either way it then sends the master one
// DONE. The master forwards every task it receives to worker (srC + scC) % workers, Savina's
// routing, and stops when it has received as many DONE as it has sent tasks. That termination is
// sound because every channel is per-sender FIFO: a worker's eight tasks reach the master before
// the DONE of the task that made them.
//
// Recursive decomposition: tasks CREATED by the work itself (8 per split; 584 at the defaults),
// relayed through one master, and the work at the leaves (512 block products of 128^3
// multiply-adds). No actor is created inside the window -- Savina's workers are created once, by
// the master, before the first task; the creation in this shape is the tasks'. Every task for one
// quarter of C carries the same (srC, scC) and goes to the same worker, which is what lets
// workers add into C without a lock (the two products of one quarter run one after the other on
// that worker).
//
// `length`    -- the matrices' dimension, a power of two. Savina's own default, 1 024.
// `threshold` -- the block count (dim^2) at or under which a task is multiplied, not split.
//                Savina's own default, 16 384: leaves of 128 x 128.
// `workers`   -- Savina's own default, 20. Where the framework places, worker w on core
//                floor(w x cores / workers) -- contiguous blocks, NOT w % cores: Savina's routing
//                sends every leaf of the defaults to a worker whose index is a multiple of 4
//                (recmatmul.md, "Placement"), which w % cores would put on one core.
// `cores`     -- worker-thread budget. `wait` -- 1 = spin, 0 = park.
//
// NOT implemented: Savina's PRIORITIES (the priority-mailbox variant); the plain actor variant
// every framework runs ignores them, as Savina's Akka one does.
inline std::map<std::string, long long> params() {
    return {{"length", 1024}, {"threshold", 16384}, {"workers", 20}, {"cores", 2}, {"wait", 1}};
}

inline constexpr long long kMaxLength  = 2048;
inline constexpr long long kMaxWorkers = 1LL << 12;

struct Config {
    std::uint32_t length{0};
    std::uint32_t threshold{0};
    std::uint32_t workers{0};
};

// The parameters, checked before anything is built: a power-of-two dimension (Savina halves it at
// every split and would drop a row of an odd one) small enough for every entry of C to stay an
// exact double (below), a threshold, a pool. Outside that, the run is refused with the harness's
// fatal exit 2 (qvo::refuse).
inline Config config(const qvo::Params &p) {
    const long long length    = p.get("length");
    const long long threshold = p.get("threshold");
    const long long workers   = p.get("workers");
    if (length < 1 || length > kMaxLength || (length & (length - 1)) != 0 || threshold < 1
        || threshold > (1LL << 30) || workers < 1 || workers > kMaxWorkers)
        qvo::refuse("savina/recmatmul: need a power-of-two 1 <= length <= "
                    + std::to_string(kMaxLength) + ", 1 <= threshold <= 2^30 and 1 <= workers <= "
                    + std::to_string(kMaxWorkers) + " (got length=" + std::to_string(length)
                    + " threshold=" + std::to_string(threshold) + " workers="
                    + std::to_string(workers) + ")");
    Config c;
    c.length    = static_cast<std::uint32_t>(length);
    c.threshold = static_cast<std::uint32_t>(threshold);
    c.workers   = static_cast<std::uint32_t>(workers);
    return c;
}

// Where the framework places: worker w on core floor(w x cores / workers).
inline unsigned core_of(std::uint32_t w, std::uint32_t workers, unsigned cores) noexcept {
    return static_cast<unsigned>(static_cast<std::uint64_t>(w) * cores / workers);
}

// Savina's MatMulConfig.WorkMessage, its priority aside: the top-left corners of the A, B and C
// blocks, the block count and the dimension.
struct Task {
    std::uint32_t srA{0}, scA{0}, srB{0}, scB{0}, srC{0}, scC{0};
    std::uint32_t blocks{0};
    std::uint32_t dim{0};
};

inline Task root(const Config &c) noexcept {
    Task t;
    t.blocks = c.length * c.length;
    t.dim    = c.length;
    return t;
}

inline bool is_leaf(const Task &t, const Config &c) noexcept { return t.blocks <= c.threshold; }

// Savina's routing: Master.sendWork.
inline std::uint32_t route(const Task &t, const Config &c) noexcept {
    return (t.srC + t.scC) % c.workers;
}

// Savina's Worker.myRecMat, the split: eight tasks of half the dimension, in Savina's order.
template <typename Emit>
void split(const Task &t, Emit &&emit) {
    const std::uint32_t z = 0, h = t.dim / 2, nb = t.blocks / 4;
    auto child = [&](std::uint32_t ra, std::uint32_t ca, std::uint32_t rb, std::uint32_t cb,
                     std::uint32_t rc, std::uint32_t cc) {
        Task k;
        k.srA    = t.srA + ra;
        k.scA    = t.scA + ca;
        k.srB    = t.srB + rb;
        k.scB    = t.scB + cb;
        k.srC    = t.srC + rc;
        k.scC    = t.scC + cc;
        k.blocks = nb;
        k.dim    = h;
        emit(k);
    };
    child(z, z, z, z, z, z);
    child(z, h, h, z, z, z);
    child(z, z, z, h, z, h);
    child(z, h, h, h, z, h);
    child(h, z, z, z, h, z);
    child(h, h, h, z, h, z);
    child(h, z, z, h, h, h);
    child(h, h, h, h, h, h);
}

// ---------------------------------------------------------------------------------------------
// The data. A, B and C are `length` x `length` doubles, each row padded by 16 doubles so a row
// starts 128 bytes after a multiple of the previous one: 8 192 bytes apart, the column walk of the
// leaf kernel would map every element of a column to one L1 set, which Savina's double[][] (rows
// that are separate JVM arrays, a header apart) does not do. The same layout in every framework.
//
// DEVIATION in the VALUES (recmatmul.md): Savina fills A[i][k] = i and B[k][j] = j, so every
// product of a quarter is the same whatever inner range it multiplies -- a task carrying the
// wrong inner offset still produces Savina's C. Here A[i][k] = i + k + 1 and B[k][j] = 2k + j + 1:
// every inner range gives a different sum, C is not symmetric, and every entry is a closed form,
//
//     C[i][j] = n.a.b + (2a + b).S1 + 2.S2,   a = i + 1, b = j + 1,
//     S1 = n(n - 1)/2,  S2 = (n - 1)n(2n - 1)/6,
//
// an integer below 2^36 at length 2 048, as is every partial sum: every addition is exact, so C
// does not depend on the order of the products, and the comparison is bitwise. The work -- the
// multiplies and adds -- is Savina's.
// ---------------------------------------------------------------------------------------------

inline constexpr std::size_t kRowPad = 16;
inline constexpr std::size_t kAlign  = 128;

struct Matrices {
    std::size_t n{0};
    std::size_t stride{0};
    struct Free {
        void operator()(double *p) const noexcept { ::operator delete[](p, std::align_val_t{kAlign}); }
    };
    std::unique_ptr<double[], Free> a, b, c;

    explicit Matrices(std::size_t length) : n(length), stride(length + kRowPad) {
        const std::size_t count = n * stride;
        auto make = [count] {
            return std::unique_ptr<double[], Free>(static_cast<double *>(
                ::operator new[](count * sizeof(double), std::align_val_t{kAlign})));
        };
        a = make();
        b = make();
        c = make();
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < stride; ++j) {
                const bool pad = j >= n;
                a[i * stride + j] = pad ? 0.0 : static_cast<double>(i + j + 1);
                b[i * stride + j] = pad ? 0.0 : static_cast<double>(2 * i + j + 1);
                c[i * stride + j] = 0.0;
            }
    }
};

// Savina's leaf, Worker.myRecMat's triple loop, operation for operation: C[i][j] += A[i][scA + k] *
// B[srB + k][j] over the block's rows i (from srC, which is always srA), columns j (from scC,
// always scB) and inner index k. Shared by all four implementations.
QVOSPEC_RECMATMUL_NOINLINE inline void multiply(const Matrices &m, const Task &t) noexcept {
    const std::size_t s  = m.stride;
    const double     *A  = m.a.get();
    const double     *B  = m.b.get();
    double           *C  = m.c.get();
    const std::size_t er = static_cast<std::size_t>(t.srC) + t.dim;
    const std::size_t ec = static_cast<std::size_t>(t.scC) + t.dim;
    for (std::size_t i = t.srC; i < er; ++i)
        for (std::size_t j = t.scC; j < ec; ++j)
            for (std::size_t k = 0; k < t.dim; ++k)
                C[i * s + j] += A[i * s + t.scA + k] * B[(t.srB + k) * s + j];
}

// ---------------------------------------------------------------------------------------------
// THE VERIFIED ANSWER
//
// The task tree is fixed by the parameters, and so is the worker every task is routed to: what
// each worker must receive, split and multiply is a closed count, and C is a closed form. What is
// compared, after the window:
//
//   * C, entry by entry, against its closed form -- a task lost on its way to the master (the run
//     then ends with that product missing), delivered twice (a product added twice), routed with
//     the wrong corners, or answered with a DONE and no product, moves it;
//   * each worker's Tally -- the tasks it received, the leaves it multiplied, the tasks it split --
//     against the tree's counts for that worker: a task delivered to the wrong worker moves two
//     tallies even when the product lands where it should;
//   * the master's counts -- the tasks it received from the workers to forward (the CREATED ones),
//     the DONEs -- against the tree's.
//
// A task or a DONE LOST on its way to a worker or from it never ends the run -- the master waits
// for a DONE per task it sent -- and the harness's deadline fails it (FAIRNESS.md section 0).
// Tallies live in 128-byte slots, one per worker (thread_ring::Tally says why); the master's
// counts in a record its handlers alone write.
// ---------------------------------------------------------------------------------------------

struct Counts {
    std::uint64_t works{0};   // tasks this worker's handler ran
    std::uint64_t leaves{0};  // of them, multiplied
    std::uint64_t splits{0};  // of them, split into eight
};
// A worker's Counts in a 128-byte slot of its own. An adapter whose workers of two threads can
// never share a line keeps its Counts in the actor and hands them to the slot at teardown (qb,
// whose actors live in their own core's arena -- thread_ring::Counts says why).
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally : Counts {};

// The master's counts. `sent` is its own (the root and every forward); `created` and `dones` are
// what its handlers received.
struct MasterCounts {
    std::uint64_t created{0};  // tasks received from the workers, to forward
    std::uint64_t dones{0};
    std::uint64_t sent{0};
};

// The tree, walked without a framework: every worker's expected Tally and the master's counts.
struct Tree {
    std::vector<Tally> tallies;
    MasterCounts       master;
    std::uint64_t      leaves{0};
};

inline Tree tree(const Config &c) {
    Tree              t;
    t.tallies.resize(c.workers);
    std::vector<Task> stack{root(c)};
    while (!stack.empty()) {
        const Task task = stack.back();
        stack.pop_back();
        Tally &w = t.tallies[route(task, c)];
        ++w.works;
        ++t.master.dones;
        ++t.master.sent;
        if (is_leaf(task, c)) {
            ++w.leaves;
            ++t.leaves;
        } else {
            ++w.splits;
            split(task, [&](const Task &k) {
                ++t.master.created;
                stack.push_back(k);
            });
        }
    }
    return t;
}

inline constexpr std::uint64_t kMatrixSalt = 0x7265636d61746d75ULL;  // "recmatmu"
inline constexpr std::uint64_t kMasterSalt = 0x7265636d61746d76ULL;

inline std::uint64_t entry(std::uint64_t i, std::uint64_t j, std::uint64_t value_bits) noexcept {
    return qvo::mix(qvo::mix(kMatrixSalt + (i << 24) + j) ^ value_bits);
}

inline std::uint64_t digest(std::uint64_t worker, const Counts &t) noexcept {
    return qvo::mix(qvo::mix(qvo::mix((worker << 40) ^ t.works) ^ t.leaves) ^ t.splits);
}

inline std::uint64_t counts_digest(const std::vector<Tally> &tallies,
                                   const MasterCounts &m) noexcept {
    std::uint64_t acc = qvo::mix(qvo::mix(kMasterSalt ^ m.created) ^ m.dones);
    for (std::size_t w = 0; w < tallies.size(); ++w) acc += digest(w, tallies[w]);
    return acc;
}

// What every adapter returns, computed after teardown from C, the workers' tallies and the
// master's counts.
inline std::uint64_t checksum(const Matrices &m, const std::vector<Tally> &tallies,
                              const MasterCounts &master) noexcept {
    std::uint64_t acc = counts_digest(tallies, master);
    for (std::size_t i = 0; i < m.n; ++i)
        for (std::size_t j = 0; j < m.n; ++j)
            acc += entry(i, j, std::bit_cast<std::uint64_t>(m.c[i * m.stride + j]));
    return acc;
}

// Every delivery the handlers ran: the workers' tasks, the master's created tasks and DONEs.
inline std::uint64_t delivered(const std::vector<Tally> &tallies,
                               const MasterCounts &master) noexcept {
    std::uint64_t n = master.created + master.dones;
    for (const Tally &t : tallies) n += t.works;
    return n;
}

// The framework-free answer: the tree's counts and C's closed form, O(length^2) -- no product is
// computed.
inline std::uint64_t expected(const qvo::Params &p) {
    const Config        c  = config(p);
    const Tree          t  = tree(c);
    const std::uint64_t n  = c.length;
    const std::uint64_t s1 = n * (n - 1) / 2;
    const std::uint64_t s2 = (n - 1) * n * (2 * n - 1) / 6;
    std::uint64_t       acc = counts_digest(t.tallies, t.master);
    for (std::uint64_t i = 0; i < n; ++i)
        for (std::uint64_t j = 0; j < n; ++j) {
            const std::uint64_t a = i + 1, b = j + 1;
            const double        v = static_cast<double>(n * a * b + (2 * a + b) * s1 + 2 * s2);
            acc += entry(i, j, std::bit_cast<std::uint64_t>(v));
        }
    return acc;
}

// The report divides by this: one leaf, a dim^3 block product (128^3 multiply-adds at the
// defaults).
inline constexpr const char *kWorkUnit = "leaf";
inline std::uint64_t work_units(const qvo::Params &p) { return tree(config(p)).leaves; }

// COUNTED at the receivers (delivered() above): a task per tree node at a worker, every node but
// the root at the master as a created task, and a DONE per node -- 3 x nodes - 1. The ready
// handshake before the window and the shutdown after it are not counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Tree t = tree(config(p));
    return t.master.sent + t.master.created + t.master.dones;
}

}  // namespace qvospec::savina::recmatmul

#endif  // QVOSPEC_SAVINA_RECMATMUL_H
