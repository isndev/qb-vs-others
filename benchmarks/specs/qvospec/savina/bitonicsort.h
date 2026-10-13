// savina/bitonicsort — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// come from a sequential run of the same network over the same input with no framework linked
// (FAIRNESS.md section 0). The rules the ping-pong spec states about the checksum (a WRAPPING SUM
// of mix(), never an XOR) apply unchanged.
//
// Savina reference: Bitonic Sort (Imam & Sarkar, AGERE 2014), one of the "parallel" benchmarks --
// BitonicSortConfig.java and BitonicSortAkkaActorBenchmark.scala, a port of StreamIt's iterative
// bitonic sort. Deviations are recorded in benchmarks/savina/bitonicsort.md.

#ifndef QVOSPEC_SAVINA_BITONICSORT_H
#define QVOSPEC_SAVINA_BITONICSORT_H

#include <qvo/harness.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace qvospec::savina::bitonicsort {

inline constexpr const char *kId = "savina/bitonicsort";

// The shape, Savina's own: a SORTING NETWORK built of actors before the window, and a stream of
// `n` values pushed through it one small message at a time. A source sends the values, then Exit;
// a kernel of log2(n) merge stages -- the last one a bitonic merge of the whole sequence -- each a
// chain of steps, each step a split-join of partitions, each partition a split-join of
// compare-exchange actors that see TWO values and send them back in order, sends them to a sink
// that checks they arrive sorted. Every split-join is a splitter dealing values round-robin, its
// workers, a joiner collecting their outputs round-robin by the order id each worker stamps on
// them, and two small adapters converting between the plain value and the stamped one. The
// topology is FIXED and the network is data-oblivious: which values each actor sees, and in which
// order each one leaves, is the same in every interleaving -- so every actor's work is fixed by the
// parameters, and checked (below).
//
// `n`     -- the values sorted, a power of two. Savina's N is 4 096; the default HERE is 1 024, a
//            DEVIATION with a recorded reason (bitonicsort.md): the reference network at N is
//            244 / 106 / 45 thousand actors at 4 096 / 2 048 / 1 024 (topology() below), and qb's
//            actor id is a 16-bit slot per VirtualCore -- 65 534 live actors on one core, so 1 024
//            is the largest network every framework in the field can hold at cores=1, and 4 096
//            would not fit two cores either. The cap is qb's and is reported as qb's.
// `max`   -- Savina's M: a value is `abs(nextLong()) % max`. Savina's default, 2^60; no deviation.
//            The generator draws from [0, 65 535], so `max` never binds above it; the first 65 536
//            draws of the full-period generator are distinct, so the input has no duplicate.
// `seed`  -- Savina's S. Savina's default, 2 048; no deviation -- the input IS Savina's.
// `cores` -- 1: the whole network on one thread; 2: for the frameworks that place, the steps cut in
//            two contiguous halves of equal message weight (placement() below); the pools place
//            every actor themselves.
// `wait`  -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"n", 1024}, {"max", 1LL << 60}, {"seed", 2048}, {"cores", 2}, {"wait", 1}};
}

using Values = std::vector<std::uint64_t>;

// Savina's PseudoRandom (edu/rice/habanero/benchmarks/PseudoRandom.java), as in quicksort.h: the
// low 16 bits of Java's wrapping `long` arithmetic, which unsigned 64-bit arithmetic reproduces.
struct PseudoRandom {
    std::uint64_t value;
    explicit PseudoRandom(long long seed) noexcept : value(static_cast<std::uint64_t>(seed)) {}
    std::uint64_t next() noexcept {
        value = (value * 1309 + 13849) & 65535;
        return value;
    }
};

// Checked before anything is built; outside the domain the run is refused (qvo::refuse, exit 2).
// 2^16 is far past every network a framework here can build, and keeps every count below below
// 2^40 and every index below 2^24 in the digest.
inline constexpr long long kMaxN = 1LL << 16;
inline void check(const qvo::Params &p) {
    const long long n   = p.get("n");
    const long long max = p.get("max");
    if (n < 2 || n > kMaxN || (n & (n - 1)) != 0 || max < 1)
        qvo::refuse("savina/bitonicsort: need n a power of two in [2, 2^16] and max >= 1 (got n="
                    + std::to_string(n) + " max=" + std::to_string(max) + ")");
}

// The source's values: IntSourceActor's `Math.abs(random.nextLong()) % maxValue`, in order.
inline Values input(const qvo::Params &p) {
    check(p);
    PseudoRandom random(p.get("seed"));
    const auto   max = static_cast<std::uint64_t>(p.get("max"));
    Values       v(static_cast<std::size_t>(p.get("n")));
    for (auto &x : v) x = random.next() % max;
    return v;
}

// ---------------------------------------------------------------------------------------------
// The network, as data
// ---------------------------------------------------------------------------------------------

// The reference's actor classes, by what they do with a message:
//   kSource     IntSourceActor          Start -> every value, then Exit
//   kForward    BitonicSortKernel, MergeStage, LastMergeStage, StepOfMerge, StepOfLastMerge,
//               PartitionBitonicSequence -- each forwards what it receives to one actor
//   kSplitter   RoundRobinSplitter      `length` values to each receiver in turn; Exit to all
//   kCompare    CompareExchange         two values -> min, max (or max, min), stamped `order`
//   kJoiner     RoundRobinJoiner        stamped values from `lanes` workers, `length` per worker
//                                       in turn, as plain values; Exit once every lane has closed
//   kValueData  ValueDataAdapter        a value stamped with `order`
//   kDataValue  DataValueAdapter        a stamped value as a plain one
//   kSink       ValidationActor         records the values; Exit ends the run
enum class Kind : std::uint8_t {
    kSource,
    kForward,
    kSplitter,
    kCompare,
    kJoiner,
    kValueData,
    kDataValue,
    kSink,
};

inline constexpr std::uint32_t kNone = 0xffffffffu;

struct Node {
    Kind          kind{Kind::kForward};
    bool          up{true};         // kCompare: true sends (min, max), false (max, min)
    std::uint32_t next{kNone};      // the one downstream actor (every kind but splitter and sink)
    std::uint32_t order{0};         // kCompare, kValueData: the order id stamped on what it sends
    std::uint32_t length{0};        // kSplitter, kJoiner: the run length per receiver / per lane
    std::uint32_t lanes{0};         // kSplitter: receivers; kJoiner: the workers it collects from
    std::uint32_t first{0};         // kSplitter: its receivers, Topology::receivers[first, +lanes)
    std::uint32_t cap{0};           // kJoiner: the values each lane sends it over the whole run
    std::uint32_t step{0};          // the pipeline step it belongs to (or feeds), for placement
};

// Index order is DEPENDENCIES FIRST: every actor's `next` and receivers have smaller indices, so an
// adapter creating actors in index order always holds the address of what it is wired to. The sink
// is 0 and the source is last. Steps are numbered in pipeline order, 0 being the first a value
// meets; an actor outside every step carries the step it feeds (the source, the kernel and a
// stage, their first step) or is fed by (the sink and its adapter, the last).
struct Topology {
    std::uint32_t              n{0};
    std::uint32_t              steps{0};
    std::vector<Node>          nodes;
    std::vector<std::uint32_t> receivers;
    static constexpr std::uint32_t kSink = 0;
    std::uint32_t source() const noexcept { return static_cast<std::uint32_t>(nodes.size() - 1); }
};

// The reference's construction, back to front, exactly as its constructors nest: the kernel's
// LastMergeStage(n) then MergeStage(P) for P = n/2 down to 2; a stage's steps for i = P/2 down to 1,
// each of length L = P / i over (n / P) * i partitions (the last stage: L = n / i over i), a
// partition's direction (i / directionCounter) % 2 == 0 within a merge stage and always ascending in
// the last; a step of length 2 is its compare-exchanges directly, any longer one a partition of L/2
// compare-exchanges per sequence. A StepOfMerge's output passes a DataValueAdapter, a
// StepOfLastMerge's joiner sends straight on, and a DataValueAdapter feeds the sink.
inline Topology topology(std::uint32_t n) {
    Topology t;
    t.n             = n;
    std::uint32_t built = 0;  // steps built so far, counted from the LAST in pipeline order
    auto add = [&t](Node node) {
        t.nodes.push_back(node);
        return static_cast<std::uint32_t>(t.nodes.size() - 1);
    };
    auto partition = [&](std::uint32_t order, std::uint32_t L, bool up, std::uint32_t next,
                         std::uint32_t step) {
        const std::uint32_t half = L / 2;
        const std::uint32_t fwd  = add({Kind::kValueData, true, next, order, 0, 0, 0, 0, step});
        const std::uint32_t join = add({Kind::kJoiner, true, fwd, 0, 1, half, 0, 2, step});
        const auto          first = static_cast<std::uint32_t>(t.receivers.size());
        std::vector<std::uint32_t> workers;
        for (std::uint32_t i = 0; i < half; ++i)
            workers.push_back(add({Kind::kCompare, up, join, i, 0, 0, 0, 0, step}));
        t.receivers.insert(t.receivers.end(), workers.begin(), workers.end());
        const std::uint32_t split = add({Kind::kSplitter, true, kNone, 0, 1, half, first, 0, step});
        return add({Kind::kForward, true, split, 0, 0, 0, 0, 0, step});
    };
    // One step: `nseq` sequences of length L; up(i) the direction of sequence i; `adapter` true
    // for a StepOfMerge (its joiner feeds a DataValueAdapter), false for a StepOfLastMerge.
    auto step = [&](std::uint32_t L, std::uint32_t nseq, const std::function<bool(std::uint32_t)> &up,
                    bool adapter, std::uint32_t next) {
        const std::uint32_t s    = built;
        const std::uint32_t out  = adapter ? add({Kind::kDataValue, true, next, 0, 0, 0, 0, 0, s}) : next;
        const std::uint32_t join = add({Kind::kJoiner, true, out, 0, L, nseq, 0, L, s});
        std::vector<std::uint32_t> workers;
        for (std::uint32_t i = 0; i < nseq; ++i)
            workers.push_back(L > 2 ? partition(i, L, up(i), join, s)
                                    : add({Kind::kCompare, up(i), join, i, 0, 0, 0, 0, s}));
        const auto first = static_cast<std::uint32_t>(t.receivers.size());
        t.receivers.insert(t.receivers.end(), workers.begin(), workers.end());
        const std::uint32_t split = add({Kind::kSplitter, true, kNone, 0, L, nseq, first, 0, s});
        ++built;
        return add({Kind::kForward, true, split, 0, 0, 0, 0, 0, s});
    };

    add({Kind::kSink, true, kNone, 0, 0, 0, 0, 0, 0});
    std::uint32_t loop = add({Kind::kDataValue, true, Topology::kSink, 0, 0, 0, 0, 0, 0});
    for (std::uint32_t i = n / 2; i >= 1; i /= 2)  // LastMergeStage(n)
        loop = step(n / i, i, [](std::uint32_t) { return true; }, false, loop);
    loop = add({Kind::kForward, true, loop, 0, 0, 0, 0, 0, built - 1});
    for (std::uint32_t P = n / 2; P >= 2; P /= 2) {  // MergeStage(P)
        for (std::uint32_t i = P / 2; i >= 1; i /= 2) {
            const std::uint32_t dir = i;
            loop = step(P / i, (n / P) * i, [dir](std::uint32_t j) { return (j / dir) % 2 == 0; },
                        true, loop);
        }
        loop = add({Kind::kForward, true, loop, 0, 0, 0, 0, 0, built - 1});
    }
    loop = add({Kind::kForward, true, loop, 0, 0, 0, 0, 0, built - 1});  // the kernel
    add({Kind::kSource, true, loop, 0, 0, 0, 0, 0, built - 1});
    t.steps = built;
    // Pipeline order: the step built last is the first a value meets. The sink and its adapter
    // carry step 0 as built -- the last step, S - 1, once flipped.
    for (Node &node : t.nodes) node.step = t.steps - 1 - node.step;
    return t;
}

// ---------------------------------------------------------------------------------------------
// The per-actor logic every adapter runs -- one implementation, so no framework's network
// differs from another's in anything but the messaging
// ---------------------------------------------------------------------------------------------

// RoundRobinSplitter: `length` values to each receiver in turn.
class Splitter {
    std::uint32_t index_{0};
    std::uint32_t run_{0};

public:
    // The position, in [0, node.lanes), of the receiver of the next value.
    std::uint32_t next(const Node &node) noexcept {
        const std::uint32_t r = index_;
        if (++run_ == node.length) {
            run_   = 0;
            index_ = index_ + 1 == node.lanes ? 0 : index_ + 1;
        }
        return r;
    }
};

// CompareExchange: the first value of a pair is held, the second sends both in order.
class Comparator {
    std::uint64_t held_{0};
    bool          holding_{false};

public:
    // True on the second value of a pair, with the two in the order the actor sends them.
    bool accept(std::uint64_t v, bool up, std::uint64_t &first, std::uint64_t &second) noexcept {
        if (!holding_) {
            holding_ = true;
            held_    = v;
            return false;
        }
        holding_               = false;
        const std::uint64_t lo = held_ <= v ? held_ : v;
        const std::uint64_t hi = held_ <= v ? v : held_;
        first                  = up ? lo : hi;
        second                 = up ? hi : lo;
        return true;
    }
};

// RoundRobinJoiner: values arrive stamped with their worker's order id, in whatever interleaving
// the framework delivers them, and leave in the round-robin order -- `length` from lane 0, then
// `length` from lane 1, ... -- so a value that arrives early waits in its lane. The reference
// buffers in one growing list per lane; here each lane is a fixed slice of one buffer sized at
// construction, outside the window, to what the run sends it (Node::cap). A lane receiving more is
// a value duplicated or misrouted, and stops the run with a named failure (qvo::fail).
class Joiner {
    Values                     buf_;
    std::vector<std::uint32_t> fill_;
    std::vector<std::uint32_t> read_;
    std::uint32_t              lanes_;
    std::uint32_t              cap_;
    std::uint32_t              length_;
    std::uint32_t              cur_{0};
    std::uint32_t              run_{0};
    std::uint32_t              exits_{0};

public:
    explicit Joiner(const Node &node)
        : buf_(static_cast<std::size_t>(node.lanes) * node.cap)
        , fill_(node.lanes)
        , read_(node.lanes)
        , lanes_(node.lanes)
        , cap_(node.cap)
        , length_(node.length) {}

    // Takes one stamped value, then hands `emit` every value the round-robin order releases.
    template <typename Emit>
    void accept(std::uint32_t lane, std::uint64_t value, Emit &&emit) {
        if (lane >= lanes_ || fill_[lane] == cap_)
            qvo::fail("savina/bitonicsort: a joiner received more values on lane "
                      + std::to_string(lane) + " than the run sends it -- a value duplicated or "
                                               "misrouted");
        buf_[static_cast<std::size_t>(lane) * cap_ + fill_[lane]++] = value;
        while (read_[cur_] < fill_[cur_]) {
            emit(buf_[static_cast<std::size_t>(cur_) * cap_ + read_[cur_]++]);
            if (++run_ == length_) {
                run_ = 0;
                cur_ = cur_ + 1 == lanes_ ? 0 : cur_ + 1;
            }
        }
    }

    // One lane's Exit; true for the last, when the joiner sends its own.
    bool close() noexcept { return ++exits_ == lanes_; }
};

// ---------------------------------------------------------------------------------------------
// The verified answer
// ---------------------------------------------------------------------------------------------

// Every actor TALLIES what it receives (Huly QB-1017's verification): how many messages of any
// kind -- the source's Start, values, stamped values, Exits -- and the wrapping sum of the values
// they carried. Both are fixed for every actor by the parameters: the network is data-oblivious, so
// a compare-exchange always sees the same two values, a step's actors always see the same range of
// the stream, and a joiner always gets one Exit per lane. Each actor's tally is one 128-byte slot of
// a vector the body owns (no two actors, whichever threads run them, write one line or one
// prefetched pair of lines -- thread-ring.h says why 128); the body reduces the slots after the
// framework is gone, OUTSIDE the window. An adapter whose actors keep the counts in their own
// state, on a line no other core's actor shares (qb: an actor lives in its core's thread arena),
// hands them to the slot at teardown.
struct Counts {
    std::uint64_t count{0};  // messages received, of any kind
    std::uint64_t sum{0};    // the values they carried, wrapping
    void value(std::uint64_t v) noexcept {
        ++count;
        sum += v;
    }
    void signal() noexcept { ++count; }
};
inline constexpr std::size_t kTallyLine = 128;
struct alignas(kTallyLine) Tally : Counts {};

// One actor's tally in the checksum: its index, count and sum, mixed, so a tally off in either
// number, or carried by another actor, moves the total.
inline std::uint64_t digest(std::uint64_t index, std::uint64_t count, std::uint64_t sum) noexcept {
    return qvo::mix(qvo::mix((index << 40) ^ count) ^ sum);
}

// What the sink received, reduced after the window: its length, the wrapping sum of a mix of every
// value (the multiset of the output, order-independent), and its DESCENTS, the positions where a
// value is below its predecessor. The input's length and multiset and no descent IS the sorted input.
struct Output {
    std::uint64_t length{0};
    std::uint64_t multiset{0};
    std::uint64_t descents{0};
};
inline Output measure(const Values &out) noexcept {
    Output o;
    o.length = out.size();
    for (std::size_t i = 0; i < out.size(); ++i) {
        o.multiset += qvo::mix(out[i] ^ 0xb170bec5d3e2b801ULL);
        o.descents += i > 0 && out[i] < out[i - 1];
    }
    return o;
}
inline std::uint64_t output_term(const Output &o) noexcept {
    return qvo::mix(0xb170bec500000001ULL ^ o.length) + o.multiset
           + qvo::mix(0xb170bec500000002ULL + o.descents);
}

// What every adapter returns, computed after teardown from its tallies and what its sink recorded.
inline std::uint64_t checksum(const std::vector<Tally> &tallies, const Values &out) noexcept {
    std::uint64_t acc = output_term(measure(out));
    for (std::size_t i = 0; i < tallies.size(); ++i)
        acc += digest(i, tallies[i].count, tallies[i].sum);
    return acc;
}
inline std::uint64_t delivered(const std::vector<Tally> &tallies) noexcept {
    std::uint64_t n = 0;
    for (const Tally &t : tallies) n += t.count;
    return n;
}

// The sequential reference: the network of topology() run on one thread, a FIFO of messages,
// through the same Splitter / Comparator / Joiner -- which values every actor receives, what the
// sink records -- and checked against the input sorted by std::sort, so the spec cannot pass a
// network that does not sort. Computed once per process and parameter set, and kept: expected(),
// expected_messages(), work_units() (which the harness also asks at the defaults, for the
// deadline), placement() and explain() read it, outside every window.
struct Reference {
    long long           n{-1};
    Topology            net;
    std::vector<Counts> counts;  // per actor
    std::uint64_t       multiset{0};
    std::uint64_t       messages{0};
};
inline const Reference &reference(const qvo::Params &p) {
    static std::map<std::tuple<long long, long long, long long>, Reference> cache;
    const auto key = std::make_tuple(p.get("n"), p.get("max"), p.get("seed"));
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    const Values in = input(p);
    Reference    r;
    r.net = topology(static_cast<std::uint32_t>(in.size()));
    const auto            size = r.net.nodes.size();
    r.counts.assign(size, Counts{});
    std::vector<Splitter>   splitters(size);
    std::vector<Comparator> comparators(size);
    std::vector<Joiner>     joiners;
    std::vector<std::uint32_t> joiner_of(size, kNone);
    for (std::size_t i = 0; i < size; ++i)
        if (r.net.nodes[i].kind == Kind::kJoiner) {
            joiner_of[i] = static_cast<std::uint32_t>(joiners.size());
            joiners.emplace_back(r.net.nodes[i]);
        }
    enum class Msg : std::uint8_t { kStart, kValue, kData, kExit };
    struct M {
        std::uint32_t dst;
        Msg           kind;
        std::uint32_t order;
        std::uint64_t v;
    };
    std::deque<M> fifo;
    Values        out;
    bool          exited = false;
    fifo.push_back({r.net.source(), Msg::kStart, 0, 0});
    while (!fifo.empty()) {
        const M m = fifo.front();
        fifo.pop_front();
        const Node &node = r.net.nodes[m.dst];
        if (m.kind == Msg::kValue || m.kind == Msg::kData) r.counts[m.dst].value(m.v);
        else r.counts[m.dst].signal();
        const bool exit = m.kind == Msg::kExit;
        switch (node.kind) {
        case Kind::kSource:
            for (const std::uint64_t v : in) fifo.push_back({node.next, Msg::kValue, 0, v});
            fifo.push_back({node.next, Msg::kExit, 0, 0});
            break;
        case Kind::kForward: fifo.push_back({node.next, m.kind, m.order, m.v}); break;
        case Kind::kSplitter:
            if (exit)
                for (std::uint32_t k = 0; k < node.lanes; ++k)
                    fifo.push_back({r.net.receivers[node.first + k], Msg::kExit, 0, 0});
            else
                fifo.push_back({r.net.receivers[node.first + splitters[m.dst].next(node)],
                                Msg::kValue, 0, m.v});
            break;
        case Kind::kCompare: {
            std::uint64_t a = 0, b = 0;
            if (exit) fifo.push_back({node.next, Msg::kExit, 0, 0});
            else if (comparators[m.dst].accept(m.v, node.up, a, b)) {
                fifo.push_back({node.next, Msg::kData, node.order, a});
                fifo.push_back({node.next, Msg::kData, node.order, b});
            }
            break;
        }
        case Kind::kJoiner:
            if (exit) {
                if (joiners[joiner_of[m.dst]].close()) fifo.push_back({node.next, Msg::kExit, 0, 0});
            } else {
                joiners[joiner_of[m.dst]].accept(m.order, m.v, [&](std::uint64_t v) {
                    fifo.push_back({node.next, Msg::kValue, 0, v});
                });
            }
            break;
        case Kind::kValueData:
            fifo.push_back({node.next, exit ? Msg::kExit : Msg::kData, exit ? 0 : node.order, m.v});
            break;
        case Kind::kDataValue:
            fifo.push_back({node.next, exit ? Msg::kExit : Msg::kValue, 0, m.v});
            break;
        case Kind::kSink:
            if (exit) exited = true;
            else out.push_back(m.v);
            break;
        }
    }
    Values sorted = in;
    std::sort(sorted.begin(), sorted.end());
    if (!exited || out != sorted)
        qvo::refuse("savina/bitonicsort: the spec's own reference network did not deliver its "
                    "input sorted -- a defect of the spec, not of a framework");
    r.multiset = measure(in).multiset;
    for (const Counts &c : r.counts) r.messages += c.count;
    r.n = p.get("n");
    return cache.emplace(key, std::move(r)).first->second;
}

inline std::uint64_t expected(const qvo::Params &p) {
    const Reference &r   = reference(p);
    std::uint64_t    acc = output_term(Output{static_cast<std::uint64_t>(r.n), r.multiset, 0});
    for (std::size_t i = 0; i < r.counts.size(); ++i)
        acc += digest(i, r.counts[i].count, r.counts[i].sum);
    return acc;
}

// Every message every actor received -- the source's Start, the values, the stamped values, the
// Exits -- COUNTED in the tallies (delivered()) and asserted exactly against the reference's.
inline std::uint64_t expected_messages(const qvo::Params &p) { return reference(p).messages; }

// The report divides by this: one message delivered, the unit of "many small messages over a fixed
// topology" -- 541 869 per repetition at the defaults.
inline constexpr const char *kWorkUnit = "message";
inline std::uint64_t work_units(const qvo::Params &p) { return expected_messages(p); }

// Placement for the frameworks that place (qb and the floor): each STEP whole on one core -- its
// splitter, partitions, compare-exchanges, joiners and adapters together -- and the steps cut into
// `cores` contiguous runs of about equal message weight (the reference's counts), so a value
// crosses a core `cores - 1` times on its way through, never inside a step. Every actor outside the
// steps goes with the step it carries (Node::step). At the defaults and cores=2 the cut falls
// before step 27 of 55.
inline std::vector<std::uint32_t> placement(const Reference &r, std::uint32_t cores) {
    const std::uint32_t        c = cores < 1 ? 1 : cores;
    std::vector<std::uint64_t> weight(r.net.steps, 0);
    std::uint64_t              total = 0;
    for (std::size_t i = 0; i < r.net.nodes.size(); ++i) {
        weight[r.net.nodes[i].step] += r.counts[i].count;
        total += r.counts[i].count;
    }
    std::vector<std::uint32_t> core_of_step(r.net.steps, 0);
    std::uint64_t              before = 0;
    for (std::uint32_t s = 0; s < r.net.steps; ++s) {
        core_of_step[s] = static_cast<std::uint32_t>(std::min<std::uint64_t>(c - 1, before * c / total));
        before += weight[s];
    }
    std::vector<std::uint32_t> out(r.net.nodes.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = core_of_step[r.net.nodes[i].step];
    return out;
}

// Called by every adapter after teardown: when the sink's sequence is not the sorted input, or an
// actor's tally is not the reference's, one line on stderr names the first difference -- the
// harness's verdict is the checksum's, this only says where to look.
inline void explain(const std::vector<Tally> &tallies, const Values &out, const qvo::Params &p) {
    const Reference &r = reference(p);
    const Output     o = measure(out);
    std::size_t      bad = 0, first = 0;
    bool             counts_right = tallies.size() == r.counts.size();
    for (std::size_t i = 0; i < tallies.size() && i < r.counts.size(); ++i) {
        if (tallies[i].count != r.counts[i].count) counts_right = false;
        if ((tallies[i].count != r.counts[i].count || tallies[i].sum != r.counts[i].sum) && bad++ == 0)
            first = i;
    }
    if (bad == 0 && counts_right && o.length == static_cast<std::uint64_t>(r.n)
        && o.multiset == r.multiset && o.descents == 0)
        return;
    std::string where = bad == 0 ? std::string() : ", the first at actor " + std::to_string(first);
    std::fprintf(stderr,
                 "savina/bitonicsort: the run is not the reference's -- output length %llu (want "
                 "%lld), %llu descents, multiset of the input: %s; %zu actor tallies off%s (every "
                 "count as the reference says: %s)\n",
                 static_cast<unsigned long long>(o.length), r.n,
                 static_cast<unsigned long long>(o.descents), o.multiset == r.multiset ? "yes" : "no",
                 bad, where.c_str(), counts_right ? "yes" : "no");
}

}  // namespace qvospec::savina::bitonicsort

#endif  // QVOSPEC_SAVINA_BITONICSORT_H
