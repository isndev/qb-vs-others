// savina/quicksort — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0) -- a sequential reference run of
// the same decomposition over the same input. The rules the ping-pong spec states about the
// checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Quicksort (Imam & Sarkar, AGERE 2014), one of the "parallel" benchmarks --
// QuickSortConfig.java and QuickSortAkkaActorBenchmark.scala. Deviations are recorded in
// benchmarks/savina/quicksort.md.

#ifndef QVOSPEC_SAVINA_QUICKSORT_H
#define QVOSPEC_SAVINA_QUICKSORT_H

#include <qvo/harness.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qvospec::savina::quicksort {

inline constexpr const char *kId = "savina/quicksort";

// The shape, Savina's own: a recursive decomposition whose DATA travels in the messages. An actor
// told Sort(data) with fewer than `threshold` values sorts them sequentially and answers its
// parent with Result(sorted). With `threshold` or more it takes the value at the middle of its
// sequence as the pivot, splits the sequence into the values below the pivot, those equal to it
// and those above it, creates a LEFT and a RIGHT child, sends each its part, and keeps the equal
// part. Each Result is joined as it arrives -- a left part before what the actor holds, a right
// part after it -- and on the third fragment (the equal part counts as the first) the actor
// answers its own parent and dies. The root answers the driver, which closes the window.
//
// Every Sort and every Result carries its values: one sequence moved from the parent to each child
// and one moved back, never shared. What a framework is asked is to create an actor per split, move
// those sequences between actors and run the splits and the leaf sorts in parallel; the copies the
// joins make are the same code for every framework (join() below).
//
// The root's two children -- the two SEEDS -- are created before the window, one for each half of
// the first split, and placed by the adapter on the cores the cell allows (seed s on core s % cores,
// for the frameworks that place); everything below a seed is created by its parent, inside the
// window, on whatever core the framework's spawn primitive puts it (savina/fib's arrangement, and
// for the same reason: qb has no cross-core spawn). The root itself, the driver and the input
// sequence are built before the window too. The tree below the root is DETERMINED by the input and
// the pivot rule, so the number of actors -- and of messages -- is fixed by the parameters and
// computed here by the sequential reference (reference()).
//
// `n`         -- values to sort. Savina's N, default 1 000 000; no deviation.
// `max`       -- Savina's M: a value is `nextLong() % max`. Savina's default, 2^60; no deviation.
//                Savina's generator (PseudoRandom below) draws from [0, 65 535], so at any `max`
//                above 65 535 it never binds: the input is 65 536 distinct values, each about
//                n / 65 536 times. Kept so a side experiment can narrow the range.
// `threshold` -- Savina's T: a sequence shorter than this is sorted sequentially. Savina's default,
//                2 048; no deviation. At the defaults the tree has 1 939 actors, 970 of them leaves,
//                21 levels deep (reference()).
// `seed`      -- Savina's S, the generator's seed. Savina's default, 1 024; no deviation -- the
//                input IS Savina's.
// `cores`     -- 1: everything on one thread; 2: the two seeds on two cores for the frameworks that
//                place (each half sorted on its own core), the pool's choice for the others.
// `wait`      -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"n", 1000000}, {"max", 1LL << 60}, {"threshold", 2048}, {"seed", 1024},
            {"cores", 2},   {"wait", 1}};
}

using Values = std::vector<std::uint64_t>;

// Savina's PseudoRandom (edu/rice/habanero/benchmarks/PseudoRandom.java): value = (value * 1309 +
// 13849) & 65535. The reference's arithmetic is Java's 64-bit `long`, wrapping; only the low 16
// bits survive the mask, and they are the same in unsigned 64-bit arithmetic, so any seed -- a
// negative one included -- reproduces the reference's sequence exactly.
struct PseudoRandom {
    std::uint64_t value;
    explicit PseudoRandom(long long seed) noexcept : value(static_cast<std::uint64_t>(seed)) {}
    std::uint64_t next() noexcept {
        value = (value * 1309 + 13849) & 65535;
        return value;
    }
};

// The parameters, checked before anything is built; outside them the run is refused with the
// harness's fatal exit 2 (qvo::refuse). `n` at least `threshold`: the root must split, so that both
// seeds are asked -- a smaller input is one actor's sequential sort, not this shape.
inline constexpr long long kMaxN = 1LL << 28;
inline void check(const qvo::Params &p) {
    const long long n         = p.get("n");
    const long long max       = p.get("max");
    const long long threshold = p.get("threshold");
    if (threshold < 1 || n < threshold || n > kMaxN || max < 1)
        qvo::refuse("savina/quicksort: need 1 <= threshold <= n <= 2^28 and max >= 1 (got n="
                    + std::to_string(n) + " threshold=" + std::to_string(threshold)
                    + " max=" + std::to_string(max) + ")");
}

// The input: Savina's randomlyInitArray(), `abs(nextLong() % M)` -- the generator's value is never
// negative, so the abs is the identity. Built by every adapter before the window, outside it.
inline Values input(const qvo::Params &p) {
    check(p);
    const auto   n   = static_cast<std::size_t>(p.get("n"));
    const auto   max = static_cast<std::uint64_t>(p.get("max"));
    PseudoRandom random(p.get("seed"));
    Values       v(n);
    for (auto &x : v) x = random.next() % max;
    return v;
}

// Where a child's sequence goes in its parent's (Savina's PositionLeft / Right / Initial).
enum class Position : std::uint32_t { kInitial = 0, kLeft = 1, kRight = 2 };

// The split of a Sort handler with `threshold` or more values: Savina's pivot (the value at
// size / 2) and its three filters, order-preserving. The reference makes three passes, each into
// a list reserved at the input's length; here one counting pass and one filling pass into three
// vectors of exactly their size -- the same three sequences.
struct Split {
    Values less, equal, greater;
};
inline Split split(const Values &data) {
    const std::uint64_t pivot = data[data.size() / 2];
    std::size_t         lt = 0, eq = 0;
    for (const std::uint64_t x : data) {
        lt += x < pivot;
        eq += x == pivot;
    }
    Split s;
    s.less.reserve(lt);
    s.equal.reserve(eq);
    s.greater.reserve(data.size() - lt - eq);
    for (const std::uint64_t x : data) {
        if (x < pivot) s.less.push_back(x);
        else if (x == pivot) s.equal.push_back(x);
        else s.greater.push_back(x);
    }
    return s;
}

// The leaf's sequential sort, in place: Savina's quicksortSeq made allocation-free -- the same
// pivot rule (the middle element) and the same three-way split, done by exchanges inside the
// leaf's own sequence (Dijkstra's partition) where the reference filters into three new lists per
// call and concatenates them back. Recursion on the smaller side, a loop on the larger: depth at
// most log2 of the leaf.
inline void sort_leaf(std::uint64_t *a, std::size_t size) noexcept {
    while (size > 1) {
        const std::uint64_t pivot = a[size / 2];
        std::size_t         lt = 0, i = 0, gt = size;
        while (i < gt) {
            if (a[i] < pivot) std::swap(a[lt++], a[i++]);
            else if (a[i] > pivot) std::swap(a[i], a[--gt]);
            else ++i;
        }
        // [0, lt) below, [lt, gt) equal, [gt, size) above.
        if (lt < size - gt) {
            sort_leaf(a, lt);
            a += gt;
            size -= gt;
        } else {
            sort_leaf(a + gt, size - gt);
            size = lt;
        }
    }
}
inline void sort_leaf(Values &data) noexcept { sort_leaf(data.data(), data.size()); }

// A Result joined into what the actor holds, Savina's ResultMessage handler: an empty part changes
// nothing; a left part goes before, a right part after, into a NEW sequence of both (the
// reference's `new ArrayList` + two `addAll`) -- the copies the decomposition pays on its way back
// up, the same code for every framework.
inline void join(Values &held, const Values &part, Position from) {
    if (part.empty()) return;
    Values joined;
    joined.reserve(held.size() + part.size());
    const Values &first  = from == Position::kLeft ? part : held;
    const Values &second = from == Position::kLeft ? held : part;
    joined.insert(joined.end(), first.begin(), first.end());
    joined.insert(joined.end(), second.begin(), second.end());
    held.swap(joined);
}

// What the run delivered, COUNTED where it was received and carried up the tree in every Result
// beside the values: an actor counts the Sort it handled, and on each Result from a child adds the
// child's counts and the one Result it just received; the driver does the same with the root's.
// A sub-tree of k actors therefore reports k Sorts and k - 1 Results, its parent adds the one it
// receives from it, and the whole run reports one Sort and one Result per actor.
struct Counts {
    std::uint64_t sorts{0};
    std::uint64_t results{0};
    void add_result(const Counts &child) noexcept {
        sorts += child.sorts;
        results += child.results + 1;
    }
};

// The answer, reduced AFTER the window from the sequence the driver received: its length, the
// wrapping sum of a mix of every value (order-independent: the multiset of the output), and its
// DESCENTS, the positions where a value is below its predecessor (0 for a sorted sequence). A
// sequence with the input's length and multiset and no descent IS the sorted input.
struct Output {
    std::uint64_t length{0};
    std::uint64_t multiset{0};
    std::uint64_t descents{0};
};
inline std::uint64_t value_digest(std::uint64_t v) noexcept {
    return qvo::mix(v ^ 0x9a1c5047d3e2b801ULL);
}
inline Output measure(const Values &out) noexcept {
    Output o;
    o.length = out.size();
    for (std::size_t i = 0; i < out.size(); ++i) {
        o.multiset += value_digest(out[i]);
        o.descents += i > 0 && out[i] < out[i - 1];
    }
    return o;
}

// One term per component, each a mix of a tag and the count, so a run off in any of them -- a
// value lost or doubled, a sequence out of order, a Sort or a Result missing or extra -- moves the
// total.
inline std::uint64_t answer(const Output &o, const Counts &c) noexcept {
    return qvo::mix(0x9a1c504700000001ULL ^ o.length) + o.multiset
           + qvo::mix(0x9a1c504700000002ULL + o.descents)
           + qvo::mix(0x9a1c504700000003ULL ^ c.sorts) + qvo::mix(0x9a1c504700000004ULL ^ c.results);
}

// The sequential reference: the input, its multiset, and the tree Savina's protocol builds over it
// -- one actor per Sort, a split for every sequence of `threshold` or more values -- walked with
// the same split() every adapter calls. Computed once per process and kept: expected(),
// expected_messages() and explain() all read it, outside every window.
struct Reference {
    long long     n{-1}, max{-1}, threshold{-1}, seed{-1};
    std::uint64_t multiset{0};
    std::uint64_t actors{0};
    std::uint64_t leaves{0};
};
inline const Reference &reference(const qvo::Params &p) {
    static Reference ref;
    if (ref.n == p.get("n") && ref.max == p.get("max") && ref.threshold == p.get("threshold")
        && ref.seed == p.get("seed"))
        return ref;
    const Values        data      = input(p);
    const auto          threshold = static_cast<std::size_t>(p.get("threshold"));
    Reference           r;
    std::vector<Values> pending;
    pending.push_back(data);
    while (!pending.empty()) {
        Values d = std::move(pending.back());
        pending.pop_back();
        ++r.actors;
        if (d.size() < threshold) {
            ++r.leaves;
            continue;
        }
        Split s = split(d);
        pending.push_back(std::move(s.less));
        pending.push_back(std::move(s.greater));
    }
    r.multiset = measure(data).multiset;
    r.n         = p.get("n");
    r.max       = p.get("max");
    r.threshold = p.get("threshold");
    r.seed      = p.get("seed");
    ref         = r;
    return ref;
}

inline std::uint64_t expected(const qvo::Params &p) {
    const Reference &r = reference(p);
    return answer(Output{static_cast<std::uint64_t>(r.n), r.multiset, 0}, Counts{r.actors, r.actors});
}

// One Sort per actor (the driver's to the root included) and one Result per actor (the root's to
// the driver included) -- COUNTED by the actors that received them and carried up the tree.
inline std::uint64_t expected_messages(const qvo::Params &p) { return 2 * reference(p).actors; }

// The report divides by this: one value sorted.
inline constexpr const char *kWorkUnit = "value";
inline std::uint64_t work_units(const qvo::Params &p) { return static_cast<std::uint64_t>(p.get("n")); }

// Called by every adapter after teardown: when the output is not the sorted input, or the counts
// are not the tree's, one line on stderr says which part is off -- the harness's verdict is the
// checksum's, this only names the fault.
inline void explain(const Output &o, const Counts &c, const qvo::Params &p) {
    const Reference &r = reference(p);
    if (o.length == static_cast<std::uint64_t>(r.n) && o.multiset == r.multiset && o.descents == 0
        && c.sorts == r.actors && c.results == r.actors)
        return;
    std::fprintf(stderr,
                 "savina/quicksort: the run is not the reference's -- length %llu (want %lld), "
                 "%llu descents, multiset of the input: %s, sorts %llu results %llu (want %llu "
                 "each)\n",
                 static_cast<unsigned long long>(o.length), r.n,
                 static_cast<unsigned long long>(o.descents), o.multiset == r.multiset ? "yes" : "no",
                 static_cast<unsigned long long>(c.sorts), static_cast<unsigned long long>(c.results),
                 static_cast<unsigned long long>(r.actors));
}

}  // namespace qvospec::savina::quicksort

#endif  // QVOSPEC_SAVINA_QUICKSORT_H
