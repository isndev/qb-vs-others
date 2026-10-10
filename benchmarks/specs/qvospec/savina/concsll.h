// savina/concsll — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum, the message count
// and the lower bounds on the observed work are plain arithmetic with no framework linked
// (FAIRNESS.md section 0). The rules the ping-pong spec states about the checksum (a WRAPPING SUM
// of mix(), never an XOR) apply unchanged.
//
// Savina reference: Concurrent Sorted Linked List (Imam & Sarkar, AGERE 2014), "concsll", of the
// "concurrency" group -- SortedListConfig.java, SortedLinkedList.java and
// SortedListAkkaActorBenchmark.scala. Deviations are recorded in benchmarks/savina/concsll.md.

#ifndef QVOSPEC_SAVINA_CONCSLL_H
#define QVOSPEC_SAVINA_CONCSLL_H

#include <qvo/harness.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The three walks are kept out of line so that each binary carries them as ONE function whose
// code can be compared across the four adapters (benchmarks/savina/concsll.md, "The measured
// window"), and so that no framework's handler can inline them into a different shape.
#if defined(_MSC_VER)
#define QVOSPEC_CONCSLL_NOINLINE __declspec(noinline)
#else
#define QVOSPEC_CONCSLL_NOINLINE __attribute__((noinline))
#endif

namespace qvospec::savina::concsll {

inline constexpr const char *kId = "savina/concsll";

// The shape: `workers` worker actors and ONE sorted-list actor. Each worker sends `messages`
// requests to the list, ONE AT A TIME -- it sends the next only when the answer to the previous
// one has arrived -- and each request is an insert ("write"), a membership test ("contains") or a
// length query ("size"), drawn by Savina's own generator. The list actor holds a real sorted
// singly-linked list (SortedList below, the reference's SortedLinkedList) and answers every request
// after WALKING it: an insert walks to its position, a contains walks until it finds the value or
// reaches the end, a size walks the whole list. When every worker has had its last answer it tells
// the master, and the master ends the run. So the shape is a hot SERVER actor -- one mailbox fed
// by every worker, one long, serial, cache-bound computation per message -- which no other
// benchmark of the suite has: chameneos' mall and bank-transaction's accounts answer in a few
// instructions.
//
// `workers`        -- Savina's NUM_ENTITIES, 20; no deviation.
// `messages`       -- requests per worker, Savina's NUM_MSGS_PER_WORKER, 8 000; no deviation.
// `write_percent`  -- Savina's WRITE_PERCENTAGE, 10; no deviation.
// `size_percent`   -- Savina's SIZE_PERCENTAGE, 1; no deviation. The two percentages are the
//                     generator's THRESHOLDS, not the mix it produces: see Script.
// `form`           -- how a worker waits for its answer, a declared axis, the same as concdict's
//                     (concsll.md, "The reply path"): 0 = an ordinary message back to the sender,
//                     the reference's own shape (Akka `sender ! new ResultMessage(...)`); 1 = the
//                     framework's request/response primitive (qb::ask, CAF request().then()),
//                     where it has one -- SObjectizer and the floor report it not applicable. Each
//                     adapter's table cell runs the faster of its forms as measured on the quiet
//                     host, and its main() says which. The result document records the value.
// `cores`          -- 1: everything on one thread; 2: the list actor alone on core 0 and the
//                     master and every worker on core 1, for the frameworks that place -- every
//                     request and every answer crosses a core, and the core that walks the list
//                     does nothing else (chameneos' mall placement; the pools place freely).
//                     Above 2 the workers spread over cores 1 .. cores-1 (worker_core); the walk
//                     stays one actor's, serial by the problem's definition.
// `wait`           -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"workers", 20}, {"messages", 8000}, {"write_percent", 10}, {"size_percent", 1},
            {"form", 0},     {"cores", 2},       {"wait", 1}};
}

// A run that breaks the protocol in a way no checksum could report in time -- a worker answered
// after its last request (its sum has already left in its report), an ask answered that nobody
// waits for -- stops here, loudly, instead of reporting a number. concdict's rule, word for word.
[[noreturn]] inline void fail(const char *what) {
    std::fprintf(stderr, "savina/concsll: protocol violated -- %s\n", what);
    std::abort();
}

// `form`, which is 0 or 1: true for the request/response primitive.
inline bool asks(const qvo::Params &p) {
    const long long v = p.get("form");
    if (v != 0 && v != 1) {
        std::fprintf(stderr, "savina/concsll: form must be 0 or 1 (got %lld)\n", v);
        std::abort();
    }
    return v == 1;
}

struct Config {
    std::uint32_t workers;
    std::uint64_t messages;
    std::int64_t  write_percent;
    std::int64_t  size_percent;

    static Config of(const qvo::Params &p) {
        return Config{static_cast<std::uint32_t>(p.get("workers")),
                      static_cast<std::uint64_t>(p.get("messages")),
                      static_cast<std::int64_t>(p.get("write_percent")),
                      static_cast<std::int64_t>(p.get("size_percent"))};
    }
};

// ---------------------------------------------------------------------------------------------
// The requests: Savina's generator, reproduced exactly
// ---------------------------------------------------------------------------------------------

// edu.rice.habanero.benchmarks.PseudoRandom -- "added to improve comparability between programming
// languages": a 16-bit linear congruential generator, value = (value * 1309 + 13849) & 65535.
// Computed in unsigned 64-bit arithmetic, whose low 16 bits are exactly Java's signed `long`'s.
class PseudoRandom {
public:
    explicit PseudoRandom(std::uint64_t seed) noexcept : _value(seed) {}

    std::int32_t next_int() noexcept {
        _value = ((_value * 1309u) + 13849u) & 65535u;
        return static_cast<std::int32_t>(_value);
    }
    // Java's nextInt(exclusive_max) = nextInt() % exclusive_max; nextInt() is in [0, 65535].
    std::int32_t next_int(std::int32_t exclusive_max) noexcept { return next_int() % exclusive_max; }

private:
    std::uint64_t _value;
};

// What a request asks. The values are the wire tags every adapter uses.
enum Kind : std::uint32_t { kWrite = 1, kContains = 2, kSize = 3 };

struct Request {
    std::uint32_t kind{kSize};
    std::int32_t  value{0};  // the value to insert or to look for; 0 for a size request
};

// Worker `w`'s request sequence -- the reference Worker's `process`, one call per request:
// `anInt = random.nextInt(100)`; below size_percent a size request, below size_percent +
// write_percent a write of `random.nextInt()`, otherwise a contains of `random.nextInt()`. The
// seed is the reference's, `id + numMessagesPerWorker + writePercent + sizePercent`.
//
// THE MIX IS THE GENERATOR'S, NOT THE NOMINAL ONE. Savina's 16-bit LCG is a full-period cycle with
// strongly correlated low bits, so the kind drawn (`% 100`) and the value drawn after it are not
// independent: at the defaults the 160 000 requests are 16 126 writes (10.08 %), 143 864 contains
// and 10 size requests (0.006 %, not 1 %). And NO contains ever looks for a value any worker
// writes, for any parameters: a value is always the generator's successor of the state that drew
// the kind, the states that draw "write" and those that draw "contains" are disjoint (their `% 100`
// differ), and the generator is a bijection on its 65 536 states -- so the values written and the
// values looked for are disjoint sets. Every contains walks the whole list and answers false.
// Those are properties of the reference's workload, reproduced exactly, and the checksum asserts
// every answer that follows from them (see asserted_result).
class Script {
public:
    Script(std::uint32_t worker, const Config &c) noexcept
        : _rng(std::uint64_t{worker} + c.messages + static_cast<std::uint64_t>(c.write_percent)
               + static_cast<std::uint64_t>(c.size_percent))
        , _write(static_cast<std::int32_t>(c.write_percent))
        , _size(static_cast<std::int32_t>(c.size_percent)) {}

    Request next() noexcept {
        const std::int32_t a = _rng.next_int(100);
        if (a < _size) return Request{kSize, 0};
        if (a < _size + _write) return Request{kWrite, _rng.next_int()};
        return Request{kContains, _rng.next_int()};
    }

private:
    PseudoRandom _rng;
    std::int32_t _write;
    std::int32_t _size;
};

// Placement for the adapters that place (qb and the floor): the list alone on core 0, the master
// on core 1, worker w on core 1 + w % (cores - 1); everything on core 0 when cores is 1.
inline constexpr std::uint32_t kListCore = 0;
inline std::uint32_t master_core(std::uint32_t cores) noexcept { return cores <= 1 ? 0u : 1u; }
inline std::uint32_t worker_core(std::uint32_t worker, std::uint32_t cores) noexcept {
    return cores <= 1 ? 0u : 1u + worker % (cores - 1);
}

// A request's identity: the worker and the request's index in that worker's sequence. Every
// request carries it, the list echoes it in the answer, and the worker checks it (reply_term).
inline std::uint64_t request_id(std::uint32_t worker, std::uint64_t seq) noexcept {
    return (std::uint64_t{worker} << 32) | (seq & 0xffffffffu);
}

// ---------------------------------------------------------------------------------------------
// The list: ONE implementation, shared by every adapter
// ---------------------------------------------------------------------------------------------

// What the list walked, by request kind, and how many contains found their value. The amount of
// walking depends on the interleaving (a contains issued after more writes walks a longer list),
// so the three walks are OBSERVATIONS, reported beside the cell; a lower bound that every
// interleaving guarantees is asserted for each (see observed_at_least), and the size walk also
// enters the checksum (answer_term, list_term). `contains_found` is NOT an interleaving
// observation: Savina's generator fixes it at 0 (Script: no contains is of a written value), and
// the answer to a contains of a value no worker writes is asserted false (asserted_result), so a
// run that found a value has already failed its checksum -- it is reported as the run's own
// confirmation of that property of the workload.
struct ListStats {
    std::uint64_t contains_walk{0};
    std::uint64_t write_walk{0};
    std::uint64_t size_walk{0};
    std::uint64_t contains_found{0};
};

// Savina's SortedLinkedList<Integer>, the same algorithm step for step: `add` inserts after every
// element not greater than the item (so equal items keep their arrival order), `contains` walks
// from the head until an element EQUALS the item -- it does not use the order to stop early, as
// the reference does not -- and `size` counts the nodes, walking the whole list every time. Its
// walks are the benchmark's work, so the list is written ONCE, here, and every adapter's list
// actor holds one of these: the walk is the same source in every framework, and out of line
// (QVOSPEC_CONCSLL_NOINLINE).
//
// Two things differ from the Java, both so that the walk's memory behaviour is the same in every
// framework and is the problem's, not a heap's: the item is stored in the node (the reference
// stores a boxed Integer, one more pointer per step), and the nodes come from the list's own
// arena -- blocks of kBlock nodes, taken in insertion order -- rather than from the process heap,
// where they would be interleaved with whatever the framework allocates between two inserts
// (CAF's messages, SObjectizer's) and the list's cache footprint would differ by framework.
class SortedList {
public:
    static constexpr std::size_t kBlock = 4096;  // 64 KiB of nodes per block

    SortedList() { grow(); }  // the first block is allocated with the actor, outside the window
    SortedList(const SortedList &)            = delete;
    SortedList &operator=(const SortedList &) = delete;
    SortedList(SortedList &&) noexcept            = default;
    SortedList &operator=(SortedList &&) noexcept = default;

    QVOSPEC_CONCSLL_NOINLINE void add(std::int32_t item) {
        Node *const   node    = make(item);
        std::uint64_t visited = 0;
        if (_head == nullptr) {
            _head = node;
        } else {
            ++visited;
            if (item < _head->item) {
                node->next = _head;
                _head      = node;
            } else {
                Node *before = _head;
                Node *after  = _head->next;
                while (after != nullptr) {
                    ++visited;
                    if (item < after->item) break;
                    before = after;
                    after  = after->next;
                }
                node->next   = before->next;
                before->next = node;
            }
        }
        _stats.write_walk += visited;
    }

    QVOSPEC_CONCSLL_NOINLINE bool contains(std::int32_t item) noexcept {
        std::uint64_t visited = 0;
        bool          found   = false;
        for (const Node *n = _head; n != nullptr; n = n->next) {
            ++visited;
            if (item == n->item) {
                found = true;
                break;
            }
        }
        _stats.contains_walk += visited;
        _stats.contains_found += found ? 1 : 0;
        return found;
    }

    // The answer IS the walk: the length it returns is exactly what it adds to size_walk, which is
    // what lets the checksum assert every size answer (answer_term, list_term).
    QVOSPEC_CONCSLL_NOINLINE std::int32_t size() noexcept {
        std::int32_t r = 0;
        for (const Node *n = _head; n != nullptr; n = n->next) ++r;
        _stats.size_walk += static_cast<std::uint64_t>(r);
        return r;
    }

    // The final contents as one number: element k of the list in order contributes content_term(k,
    // item). Called once, after the window (the master's Finish), never on the hot path.
    std::uint64_t fold() const noexcept;

    const ListStats &stats() const noexcept { return _stats; }

private:
    struct Node {
        std::int32_t item;
        Node        *next;
    };

    Node *make(std::int32_t item) {
        if (_used == kBlock) grow();
        Node *const n = &_blocks.back()[_used++];
        n->item       = item;
        n->next       = nullptr;
        return n;
    }

    void grow() {
        _blocks.push_back(std::make_unique<Node[]>(kBlock));
        _used = 0;
    }

    std::vector<std::unique_ptr<Node[]>> _blocks;
    std::size_t                          _used{0};
    Node                                *_head{nullptr};
    ListStats                            _stats{};
};

// Element k (0-based, in list order) of the final contents. Position-weighted, so the fold asserts
// the ORDER as well as the multiset: an element missing, duplicated or out of place moves it.
inline std::uint64_t content_term(std::uint64_t k, std::int32_t item) noexcept {
    return qvo::mix(qvo::mix(k + 1) + static_cast<std::uint32_t>(item));
}

inline std::uint64_t SortedList::fold() const noexcept {
    std::uint64_t acc = 0;
    std::uint64_t k   = 0;
    for (const Node *n = _head; n != nullptr; n = n->next) acc += content_term(k++, n->item);
    return acc;
}

// ---------------------------------------------------------------------------------------------
// The verified answer
// ---------------------------------------------------------------------------------------------

// Which values ANY worker writes, indexed by value (the generator's values are in [0, 65535]).
// Built once per repetition, before the window, and read-only while it runs: a worker needs it to
// know which contains answers every interleaving fixes (asserted_result).
inline std::vector<std::uint8_t> written_values(const Config &c) {
    std::vector<std::uint8_t> written(65536, 0);
    for (std::uint32_t w = 0; w < c.workers; ++w) {
        Script s(w, c);
        for (std::uint64_t j = 0; j < c.messages; ++j) {
            const Request r = s.next();
            if (r.kind == kWrite) written[static_cast<std::uint32_t>(r.value) & 0xffffu] = 1;
        }
    }
    return written;
}

// What every interleaving fixes about the answer to `own`, given the answer that came back:
//   a write    -- the list answers the value it inserted (the reference's ResultMessage): fixed;
//   a contains -- of a value NO worker ever writes: false, whatever the order -- fixed. Of a value
//                 some worker writes, two cases are fixed as well -- the asker wrote it earlier
//                 (true: that write was applied before the asker could ask again) and only the
//                 asker writes it, later (false) -- and the rest depends on whether another
//                 worker's write came first. This rule asserts none of the three, the
//                 conservative choice: with Savina's generator no contains is of a written value
//                 (Script), so it loses nothing, and the rule does not depend on the generator --
//                 the table is computed, not assumed;
//   a size     -- the length at that moment: not fixed, so not asserted here. Every size answer is
//                 asserted all the same, against the list's own walk (answer_term), and is at
//                 least the worker's own earlier writes, which the size walk's lower bound asserts.
// A fixed answer enters the checksum as it ARRIVED; anything else as 0.
inline std::int32_t asserted_result(const Request &own, std::int32_t result,
                                    const std::vector<std::uint8_t> &written) noexcept {
    switch (own.kind) {
    case kWrite: return result;
    case kContains: return written[static_cast<std::uint32_t>(own.value) & 0xffffu] ? 0 : result;
    default: return 0;
    }
}

// The answer expected() assumes for `own`: the fixed answer where there is one, 0 otherwise.
inline std::int32_t expected_result(const Request &own) noexcept {
    return own.kind == kWrite ? own.value : 0;
}

// The term a worker adds per answer, before the size length answer_term adds to it. `own_id` /
// `own_kind` are what the worker ASKED (its own number and the request's index in its sequence,
// which the worker knows without reading the answer); `carried_id` / `carried_kind` are what the
// answer SAYS it answers. In a correct run they are equal, so the term is fixed by the request
// alone; an answer delivered to the wrong worker, a duplicated answer (every later answer of that
// worker is then one index off), or an answer of the wrong kind changes it -- and so does a wrong
// fixed answer (asserted_result).
inline std::uint64_t reply_term(std::uint64_t own_id, std::uint32_t own_kind, std::uint64_t carried_id,
                                std::uint32_t carried_kind, std::int32_t asserted) noexcept {
    const std::uint64_t carried = qvo::mix(carried_id + 0x5851f42d4c957f2dULL);
    return qvo::mix(qvo::mix(own_id + 1) + 0x9e3779b97f4a7c15ULL * carried
                    + (std::uint64_t{own_kind} << 40) + (std::uint64_t{carried_kind} << 48)
                    + static_cast<std::uint32_t>(asserted));
}

// What a worker adds for the answer `result` to its request `own` (identity `own_id`), the answer
// saying it answers `carried_id` / `carried_kind`: reply_term with the fixed answer where there is
// one, plus -- for a size request -- the length the answer reports. A length is not fixed by the
// interleaving, but SortedList::size adds to the size walk exactly the length it answers, so the
// size answers the workers receive sum to the list's size walk in EVERY interleaving; list_term
// subtracts that walk, and a size answer altered on its way, or not taken from the walk, is left
// in the checksum (a sum: it sees any one altered length, not two alterations that cancel).
inline std::uint64_t answer_term(std::uint64_t own_id, const Request &own, std::uint64_t carried_id,
                                 std::uint32_t carried_kind, std::int32_t result,
                                 const std::vector<std::uint8_t> &written) noexcept {
    const std::uint64_t length = own.kind == kSize ? static_cast<std::uint32_t>(result) : 0u;
    return reply_term(own_id, own.kind, carried_id, carried_kind, asserted_result(own, result, written))
           + length;
}

// What the list adds for every request it RECEIVES: the request's identity, kind and payload as
// they arrived (a size request carries 0). The workers' terms check what comes back; this checks
// what went out -- a contains of a value nobody writes answers false whatever value it carried,
// so without it a payload altered on its way to the list would still verify.
inline std::uint64_t request_term(std::uint64_t id, std::uint32_t kind, std::int32_t value) noexcept {
    return qvo::mix(qvo::mix(id + 0x2545f4914f6cdd1dULL) + (std::uint64_t{kind} << 32)
                    + static_cast<std::uint32_t>(value));
}

// What the list reports after the window: its final contents' fold, plus the request terms it
// added (`requests`), minus every size step it walked (the workers' answer_terms add them back).
// Called once, after the window, never on the hot path.
inline std::uint64_t list_term(const SortedList &list, std::uint64_t requests) noexcept {
    return list.fold() + requests - list.stats().size_walk;
}

// Every worker adds answer_term for each of its `messages` answers and reports the sum to the
// master with its last one; the list reports list_term. Order-independent: the workers' sums are
// fixed by their own sequences except for the size answers, which list_term cancels; the request
// terms by the requests, whatever order they arrive in; and the final contents by the multiset of
// writes, which no interleaving changes (each worker's writes are its own, and every write is
// applied exactly once). A write lost or applied twice moves the fold; a request whose identity,
// kind or payload changed on its way, or that the list received twice, moves the request terms;
// an answer lost hangs its worker (one request in flight per worker -- the run cannot end), and
// every other corruption of an answer moves the worker's sum.
inline std::uint64_t expected(const qvo::Params &p) {
    const Config              c = Config::of(p);
    std::uint64_t             acc = 0;
    std::vector<std::int32_t> contents;
    for (std::uint32_t w = 0; w < c.workers; ++w) {
        Script s(w, c);
        for (std::uint64_t j = 0; j < c.messages; ++j) {
            const Request       r  = s.next();
            const std::uint64_t id = request_id(w, j);
            acc += reply_term(id, r.kind, id, r.kind, expected_result(r));
            acc += request_term(id, r.kind, r.value);
            if (r.kind == kWrite) contents.push_back(r.value);
        }
    }
    std::sort(contents.begin(), contents.end());
    for (std::size_t k = 0; k < contents.size(); ++k) acc += content_term(k, contents[k]);
    return acc;
}

// Inside the window: `workers` DoWork received by the workers, `workers x messages` requests
// received by the list and as many answers received by the workers, and `workers` Ends received
// by the master. Counted at the receivers: a worker counts its DoWork and its answers and carries
// the count in its End; the list counts its requests and reports the count with its contents,
// after the window; the master counts the Ends. The readiness handshake before the window and the
// Finish / Report exchange after it are not counted. form=1 sends the same requests and answers,
// so the count is the same.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Config c = Config::of(p);
    return 2 * std::uint64_t{c.workers} + 2 * std::uint64_t{c.workers} * c.messages;
}

// The caveat every adapter carries, written once so it cannot drift between them.
inline constexpr const char *kSharedListCaveat =
    "the list walk is the same source in every framework (qvospec SortedList: out of line, nodes from its "
    "own arena) and dominates the window by construction -- each request walks up to the whole list, "
    "thousands of nodes, against one message each way. What differs between the cells is the cost of "
    "workers x messages request/answer round trips, where the list actor runs (and whether it keeps its "
    "cache), and how far the interleaving lets the list grow before each walk: the walks are reported "
    "beside the cell as observations, each with an asserted lower bound";

// The report divides by this: one request -- the request to the list, its walk, and its answer.
inline constexpr const char *kWorkUnit = "request";
inline std::uint64_t work_units(const qvo::Params &p) {
    const Config c = Config::of(p);
    return std::uint64_t{c.workers} * c.messages;
}

// ---------------------------------------------------------------------------------------------
// Observations and their asserted lower bounds
// ---------------------------------------------------------------------------------------------

inline constexpr const char *kObsContainsWalk  = "contains_walk";
inline constexpr const char *kObsWriteWalk     = "write_walk";
inline constexpr const char *kObsSizeWalk      = "size_walk";
inline constexpr const char *kObsContainsFound = "contains_found";

// What every adapter reports, from its list's stats, after the window.
inline std::map<std::string, std::uint64_t> observations(const ListStats &s) {
    return {{kObsContainsWalk, s.contains_walk},
            {kObsWriteWalk, s.write_walk},
            {kObsSizeWalk, s.size_walk},
            {kObsContainsFound, s.contains_found}};
}

// The walking EVERY interleaving does. When worker w's request j reaches the list, every write w
// made before it has been applied (w waited for each one's answer, and the list answers a write
// after inserting it) -- the only thing the order of the run cannot take away. So:
//   a write of v walks at least past every node not greater than v, so at least w's own earlier
//     writes not greater than v;
//   a contains of a value no worker writes walks the whole list, so at least w's own earlier writes;
//   a size walks the whole list, so at least w's own earlier writes.
// The bounds exist for an adapter that answers without the list (a contains answered from a hash
// set still reaches the right checksum): its walk is then short of them, and the repetition fails
// like a wrong checksum. At the defaults they are about a twentieth of what a run walks.
struct WalkBounds {
    std::uint64_t contains_walk{0};
    std::uint64_t write_walk{0};
    std::uint64_t size_walk{0};
};

inline WalkBounds walk_bounds(const Config &c) {
    const std::vector<std::uint8_t> written = written_values(c);
    WalkBounds                      b;
    std::vector<std::int32_t>       own;  // this worker's earlier writes, sorted
    for (std::uint32_t w = 0; w < c.workers; ++w) {
        own.clear();
        Script s(w, c);
        for (std::uint64_t j = 0; j < c.messages; ++j) {
            const Request r = s.next();
            switch (r.kind) {
            case kWrite: {
                const auto at = std::upper_bound(own.begin(), own.end(), r.value);
                b.write_walk += static_cast<std::uint64_t>(at - own.begin());
                own.insert(at, r.value);
                break;
            }
            case kContains:
                if (!written[static_cast<std::uint32_t>(r.value) & 0xffffu]) b.contains_walk += own.size();
                break;
            default: b.size_walk += own.size(); break;
            }
        }
    }
    return b;
}

inline std::map<std::string, std::function<std::uint64_t(const qvo::Params &)>> observed_at_least() {
    return {{kObsContainsWalk, [](const qvo::Params &p) { return walk_bounds(Config::of(p)).contains_walk; }},
            {kObsWriteWalk, [](const qvo::Params &p) { return walk_bounds(Config::of(p)).write_walk; }},
            {kObsSizeWalk, [](const qvo::Params &p) { return walk_bounds(Config::of(p)).size_walk; }}};
}

}  // namespace qvospec::savina::concsll

#endif  // QVOSPEC_SAVINA_CONCSLL_H
