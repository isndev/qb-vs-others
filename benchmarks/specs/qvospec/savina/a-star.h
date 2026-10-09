// savina/a-star — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header: the graph, the search step every worker
// runs and the expected checksum are plain C++ with no framework linked (FAIRNESS.md section 0).
// The rules the ping-pong spec states about the checksum (a WRAPPING SUM of mix(), never an XOR)
// apply unchanged.
//
// Savina reference: Guided Search, the "astar" benchmark of the parallelism group (Imam & Sarkar,
// AGERE 2014) -- GuidedSearchConfig.java and GuidedSearchAkkaActorBenchmark.scala under
// shamsimam/savina's src/main/{java,scala}/edu/rice/habanero/benchmarks/astar/. Deviations are
// recorded in benchmarks/savina/a-star.md.

#ifndef QVOSPEC_SAVINA_A_STAR_H
#define QVOSPEC_SAVINA_A_STAR_H

#include <qvo/harness.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace qvospec::savina::a_star {

inline constexpr const char *kId = "savina/a-star";

// The shape: a master and `workers` search workers explore one shared graph. The master hands a
// node to a worker (round-robin over the workers, Savina's `numWorkSent % numWorkers`); the worker
// walks the graph breadth-first from it, CLAIMING every neighbour it reaches -- one atomic
// compare-and-swap on the neighbour's parent slot, so each node is claimed by exactly one worker
// -- and does `work` units of busy computation per node it searches. After `threshold` nodes it
// stops and sends every node still in its local queue back to the master as one message each; the
// master redistributes them, round-robin again. Every work message is acknowledged; when the
// master has received as many acknowledgements as it sent work messages, the graph is exhausted.
//
// It is the Savina shape with WORK per message: what a framework pays to move a node from a worker
// to the master and on to another worker is measured against a fixed amount of computation per
// node, the dynamic redistribution of that computation over the cores is the framework's (or its
// placement's), and the master is a real fan-in / fan-out hot spot.
//
// `workers`   -- search workers. Savina's own default, 20.
// `grid`      -- the graph is a grid x grid x grid lattice. Savina's own default, 30 (27 000 nodes,
//               20 515 reachable from the origin). Savina caps it at 248; so does this spec.
// `threshold` -- nodes one work message searches before it hands its frontier back. Savina's own
//               default, 1 024.
// `work`      -- busy-work units per searched node: qvo::spin_work iterations, Savina's busyWait()
//               loop count (100). A DEVIATION in kind, recorded in a-star.md: Savina's unit is a
//               Math.random() call, whose generator is ONE AtomicLong every worker CASes.
// `cores`     -- worker budget. The master is actor 0, search worker w is actor w + 1, and actor a
//               lives on core a % cores for the frameworks that place.
// `wait`      -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"workers", 20}, {"grid", 30}, {"threshold", 1024}, {"work", 100},
            {"cores", 2},    {"wait", 1}};
}

// java.util.Random, transcribed: the 48-bit LCG of its documentation, seeded through the same
// scramble, so `next_boolean()` returns the bit sequence Savina's `new Random(123456L)` returns.
class JavaRandom {
public:
    explicit JavaRandom(std::int64_t seed) noexcept
        : seed_((static_cast<std::uint64_t>(seed) ^ kMultiplier) & kMask) {}

    // Random.nextBoolean() == next(1) != 0, and next(1) is the top bit of the 48-bit state.
    bool next_boolean() noexcept {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask;
        return (seed_ >> 47) != 0;
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend     = 0xBULL;
    static constexpr std::uint64_t kMask       = (1ULL << 48) - 1;
    std::uint64_t                  seed_;
};

// Savina's graph, bit for bit: GuidedSearchConfig.initializeData() transcribed. Node (i, j, k) has
// id g*g*i + g*j + k; its candidate neighbours are the six lattice steps (di, dj, dk) in {0,1}^3
// other than (0,0,0) and (1,1,1), each clamped to the grid, visited in Java's i-j-k loop order and
// kept with probability 1/2 -- except the seventh, (1,1,0), which is kept unconditionally when no
// earlier candidate was (and then draws NO random bit: Java's `||` short-circuits). A clamped step
// that lands on the node itself or on a neighbour already kept is dropped.
//
// The nodes draw their bits in the order Savina's HashMap<Integer, GridNode> iterates them: by
// bucket, an Integer key hashing to itself and HashMap spreading it to `h ^ (h >>> 16)` in a table
// larger than the largest id, so one node per bucket, in ascending `id ^ (id >> 16)`. That is
// ascending id below 65 536 -- every grid up to 40, the default included -- and a fixed
// permutation above. Checked against Savina's own Java at grid 2 to 100 (a-star.md).
//
// Every edge goes to a node with no smaller coordinate, so the origin (id 0) has no predecessor and
// the reachable set is the same whoever claims what. Read-only after construction: shared by every
// worker of every framework without synchronisation.
class Grid {
public:
    static constexpr std::uint32_t kOrigin  = 0;
    static constexpr std::uint32_t kMaxSide = 248;  // Savina: (MessagePriority.values().length - 1) * 8

    explicit Grid(std::uint32_t side) : side_(side) {
        if (side < 2 || side > kMaxSide) {
            std::fprintf(stderr, "savina/a-star: grid=%u is outside Savina's [2, %u]\n", side,
                         kMaxSide);
            std::abort();
        }
        const std::uint32_t g = side;
        const std::uint32_t n = g * g * g;

        // At most six neighbours a node: drawn into fixed slots in HashMap order, compacted into
        // id order below.
        std::vector<std::uint32_t> slots(static_cast<std::size_t>(n) * 6);
        std::vector<std::uint8_t>  kept(n, 0);

        // `id ^ (id >> 16)` is its own inverse on 32 bits, so walking the keys in ascending order
        // and inverting each walks the ids in HashMap order; every key of an id below n is below
        // the power of two that covers n.
        std::uint32_t keys = 1;
        while (keys < n) keys <<= 1;

        JavaRandom random{123456};
        for (std::uint32_t key = 0; key < keys; ++key) {
            const std::uint32_t id = key ^ (key >> 16);
            if (id >= n) continue;
            const std::uint32_t gi    = id / (g * g);
            const std::uint32_t gj    = (id / g) % g;
            const std::uint32_t gk    = id % g;
            std::uint32_t      *mine  = slots.data() + static_cast<std::size_t>(id) * 6;
            std::uint8_t       &count = kept[id];
            std::uint32_t       iter  = 0;
            for (std::uint32_t di = 0; di < 2; ++di)
                for (std::uint32_t dj = 0; dj < 2; ++dj)
                    for (std::uint32_t dk = 0; dk < 2; ++dk) {
                        ++iter;
                        if (iter == 1 || iter == 8) continue;
                        if (!((iter == 7 && count == 0) || random.next_boolean())) continue;
                        const std::uint32_t to = g * g * std::min(g - 1, gi + di) +
                                                 g * std::min(g - 1, gj + dj) +
                                                 std::min(g - 1, gk + dk);
                        if (to == id || std::find(mine, mine + count, to) != mine + count)
                            continue;
                        mine[count++] = to;
                    }
        }

        first_.assign(static_cast<std::size_t>(n) + 1, 0);
        for (std::uint32_t id = 0; id < n; ++id) first_[id + 1] = first_[id] + kept[id];
        edges_.resize(first_[n]);
        for (std::uint32_t id = 0; id < n; ++id)
            std::copy_n(slots.data() + static_cast<std::size_t>(id) * 6, kept[id],
                        edges_.data() + first_[id]);
    }

    std::uint32_t nodes() const noexcept { return side_ * side_ * side_; }

    std::span<const std::uint32_t> neighbours(std::uint32_t node) const noexcept {
        return {edges_.data() + first_[node], edges_.data() + first_[node + 1]};
    }

private:
    std::uint32_t              side_;
    std::vector<std::uint32_t> first_;  // CSR: node n's neighbours are edges_[first_[n], first_[n+1])
    std::vector<std::uint32_t> edges_;
};

// The claim slots: Savina's `GridNode.parentInPath`, an AtomicReference compareAndSet from null.
// Slot n holds 0 while n is unclaimed and `parent + 1` once claimed. Shared mutable state across
// the workers -- the one thing in this repository that is not passed by message, because it is
// the one thing Savina's own implementation does not pass by message; every framework and the
// floor go through this same class. A claim publishes nothing (the graph is immutable and the
// claimed node travels by message), so only the read-modify-write's atomicity is needed: relaxed.
class Claims {
public:
    // Value-initialised: every slot starts at 0, unclaimed.
    explicit Claims(std::uint32_t nodes)
        : parent_(std::make_unique<std::atomic<std::uint32_t>[]>(nodes)) {}

    bool claim(std::uint32_t node, std::uint32_t by) noexcept {
        std::uint32_t unclaimed = 0;
        return parent_[node].compare_exchange_strong(unclaimed, by + 1, std::memory_order_relaxed,
                                                     std::memory_order_relaxed);
    }

private:
    std::unique_ptr<std::atomic<std::uint32_t>[]> parent_;
};

// What searching one node yields: its busy work, folded. spin_work starts from `seed | 1`, so the
// seed is 2n + 1 -- n and n ^ 1 would otherwise start from the same state -- and spin_work is a
// chain of bijections, so two different nodes never yield the same value.
inline std::uint64_t node_value(std::uint32_t node, int work) noexcept {
    return qvo::mix(qvo::spin_work(2ULL * node + 1ULL, work));
}

// One work message's search, identical in every framework: a breadth-first walk from `root` over
// a FIFO in `queue` (the worker's own, reused across messages), at most `threshold` nodes. For
// each node: its busy work, then a claim on each of its neighbours in order, a claimed neighbour
// joining the queue. Savina's worker stops the run when it claims the target; this one does not
// (a-star.md, "the one deviation in kind"), so every node in the queue after the loop is handed
// back -- `emit(node)`, in queue order -- and the caller acknowledges the message with the result.
struct Chunk {
    std::uint64_t chk{0};    // wrapping sum of node_value() over the nodes searched
    std::uint64_t nodes{0};  // how many were searched
};

template <typename Emit>
Chunk search(const Grid &grid, Claims &claims, std::uint32_t root, std::uint32_t threshold,
             int work, std::vector<std::uint32_t> &queue, Emit &&emit) {
    Chunk       c;
    std::size_t head = 0;
    queue.clear();
    queue.push_back(root);
    while (head < queue.size() && c.nodes < threshold) {
        const std::uint32_t node = queue[head++];
        ++c.nodes;
        c.chk += node_value(node, work);
        for (const std::uint32_t next : grid.neighbours(node))
            if (claims.claim(next, node)) queue.push_back(next);
    }
    for (; head < queue.size(); ++head) emit(queue[head]);
    return c;
}

// Every node reachable from the origin, in breadth-first order. Each of them is searched exactly
// once in a correct run, whoever claims it: the origin by the first work message, every other node
// by the worker that claimed it or, handed back, by the worker the master forwards it to.
inline std::vector<std::uint32_t> reachable(const Grid &grid) {
    std::vector<std::uint8_t>  seen(grid.nodes(), 0);
    std::vector<std::uint32_t> order{Grid::kOrigin};
    seen[Grid::kOrigin] = 1;
    for (std::size_t i = 0; i < order.size(); ++i)
        for (const std::uint32_t next : grid.neighbours(order[i]))
            if (!seen[next]) {
                seen[next] = 1;
                order.push_back(next);
            }
    return order;
}

// The wrapping sum of node_value() over the reachable set. Each worker's acknowledgement carries
// the sum over the nodes its message searched, and the master adds them up: a work message lost
// or duplicated anywhere -- master to worker, or worker back to master -- drops a node from the
// sum or counts one twice, and every node's term is distinct.
inline std::uint64_t expected(const qvo::Params &p) {
    const Grid    grid(static_cast<std::uint32_t>(p.get("grid")));
    const int     work = static_cast<int>(p.get("work"));
    std::uint64_t acc  = 0;
    for (const std::uint32_t node : reachable(grid)) acc += node_value(node, work);
    return acc;
}

// The report divides by this: one node searched -- claimed, its busy work done, its neighbours
// tried -- with whatever messages moving it between the workers cost.
inline constexpr const char *kWorkUnit = "node";
inline std::uint64_t work_units(const qvo::Params &p) {
    return reachable(Grid(static_cast<std::uint32_t>(p.get("grid")))).size();
}

// NO expected_messages. How many work messages a run sends is decided by which worker wins each
// claim: a node claimed late in a worker's chunk is handed back as a message, the same node claimed
// early by another is searched in place. It is fixed on one thread and varies from run to run on
// two (a-star.md gives the measured spread). What is fixed is the WORK -- every reachable node
// searched exactly once -- and the checksum asserts that, node by node.
//
// What the checksum does NOT prove is that the work was REDISTRIBUTED: an adapter whose workers
// ignored `threshold` would search the whole graph from the first work message and still verify.
// The bound below is the framework-free half of closing that: one work message searches at most
// `threshold` nodes and every reachable node is searched exactly once, so a run sends at least
// ceil(reachable / threshold) work messages -- 21 at the defaults (20 515 / 1 024 = 20.03). Every
// work message is acknowledged, so the master's count of work messages sent -- the origin's plus one
// per node a worker handed back -- is the count to hold against it. Every adapter reports that count
// as the observation kObservedWorkMessages (qvo::Answer::observed) and every adapter's main()
// declares this function as its lower bound (qvo::Spec::observed_at_least), so the harness ASSERTS
// it like the checksum: a run that reports fewer, or none, fails with no timing. The harness checks
// it after the checksum has verified, i.e. once "every reachable node searched exactly once" is
// established -- which is what makes "fewer than ceil(reachable / threshold) messages" mean "some
// message searched more than threshold nodes". Measured runs send far more (a-star.md): the bound
// is a floor on the shape, not an estimate of the traffic, and the observed value beside the cell
// says how far above it each framework's interleaving landed.
inline std::uint64_t min_work_messages(const qvo::Params &p) {
    const auto threshold = static_cast<std::uint64_t>(p.get("threshold"));
    if (threshold == 0) {
        std::fprintf(stderr, "savina/a-star: threshold=0 searches nothing\n");
        std::abort();
    }
    return (work_units(p) + threshold - 1) / threshold;
}

// The observation every adapter reports (qvo::Answer::observed) and asserts against
// min_work_messages (qvo::Spec::observed_at_least): the work messages the master sent in the
// repetition, the origin's included -- one more than the frontier nodes the workers handed back,
// since the master relays each handed-back node as exactly one work message.
inline constexpr const char *kObservedWorkMessages = "work_messages";

}  // namespace qvospec::savina::a_star

#endif  // QVOSPEC_SAVINA_A_STAR_H
