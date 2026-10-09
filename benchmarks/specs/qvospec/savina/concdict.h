// savina/concdict — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Concurrent Dictionary (Imam & Sarkar, AGERE 2014), one of the "concurrency"
// benchmarks -- concdict/DictionaryConfig.java and DictionaryAkkaActorBenchmark.scala. Deviations
// are recorded in benchmarks/savina/concdict.md.

#ifndef QVOSPEC_SAVINA_CONCDICT_H
#define QVOSPEC_SAVINA_CONCDICT_H

#include <qvo/harness.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace qvospec::savina::concdict {

inline constexpr const char *kId = "savina/concdict";

// The shape, Savina's own: ONE dictionary actor holds a hash map, pre-filled with every key below
// `keys` mapped to itself. `workers` worker actors each send it `messages` requests, ONE AT A TIME:
// a worker sends its next request only once the dictionary has answered the previous one. A
// request is a write (`write` percent of them: store `value` under `key`, answered with the value
// stored) or a read (answered with the value under `key`). A worker that has its last answer tells
// the MASTER it is done; once every worker is, the master ends the dictionary.
//
// So the run is `workers` request/reply chains, all served by one actor -- the shape that measures
// what a framework's answer-the-sender path costs (qb's `reply()` / `qb::ask`, CAF's handler result
// / `request().then()`, SObjectizer's message sent back on a direct mbox) on top of a 20-writer
// fan-in into one mailbox: the dictionary is the serial bottleneck, and how much of each round trip
// a framework overlaps with the others' is the measurement.
//
// `workers`  -- Savina's own default, 20 (NUM_ENTITIES); no deviation.
// `messages` -- requests per worker. Savina's own default, 10 000 (NUM_MSGS_PER_WORKER); no
//               deviation. 200 000 round trips a repetition.
// `write`    -- the percentage of requests that write. Savina's own default, 10; no deviation.
// `keys`     -- the dictionary's key space, pre-filled. Savina's own DATA_LIMIT,
//               Integer.MAX_VALUE / 4 096 = 524 287; no deviation. Which keys a worker uses IS a
//               deviation -- see operation().
// `form`     -- how a worker waits for its answer, a declared axis (concdict.md, "The reply path"):
//               0 = an ordinary message back to the sender, the reference's own shape (Akka
//               `sender ! result`); 1 = the framework's request/response primitive (qb::ask, CAF
//               request().then()), where it has one -- SObjectizer and the floor report it not
//               applicable. Each adapter's table cell runs the faster of its forms as measured on the
//               quiet host, and its main() says which. The result document records the value.
// `cores`    -- 1: everything on one thread; 2: for the frameworks that place, the master and the
//               dictionary on core 0 and worker w on core (1 + w) % cores -- the dictionary shares
//               its core with half the workers, the other half is one pipe away.
// `wait`     -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"workers", 20}, {"messages", 10000}, {"write", 10}, {"keys", 524287},
            {"form", 0},     {"cores", 2},        {"wait", 1}};
}

// A run that breaks the protocol in a way no checksum could report in time -- a worker answered
// after its last request, an ask answered that nobody waits for, a key outside the dictionary --
// stops here, loudly, instead of reporting a number.
[[noreturn]] inline void fail(const char *what) {
    std::fprintf(stderr, "savina/concdict: protocol violated -- %s\n", what);
    std::abort();
}

// The parameters, read and checked once.
struct Shape {
    std::uint32_t workers{0};
    std::uint64_t messages{0};
    std::uint32_t write{0};       // percent
    std::uint32_t keys{0};
    std::uint32_t per_worker{0};  // keys / workers: the slots of one worker's partition
};

inline Shape shape(const qvo::Params &p) {
    const long long workers  = p.get("workers");
    const long long messages = p.get("messages");
    const long long write    = p.get("write");
    const long long keys     = p.get("keys");
    // `messages` is bounded so that a request index and a worker's received count fit 32 bits in
    // every adapter's message encoding (the floor packs them beside a worker index).
    if (workers < 1 || messages < 1 || messages >= (1LL << 31) || write < 0 || write > 100 ||
        keys < workers || keys > 0xffffffffLL) {
        std::fprintf(stderr,
                     "savina/concdict: need workers >= 1, 1 <= messages < 2^31, 0 <= write <= 100, "
                     "workers <= keys < 2^32 (got workers=%lld messages=%lld write=%lld keys=%lld)\n",
                     workers, messages, write, keys);
        std::abort();
    }
    Shape s;
    s.workers    = static_cast<std::uint32_t>(workers);
    s.messages   = static_cast<std::uint64_t>(messages);
    s.write      = static_cast<std::uint32_t>(write);
    s.keys       = static_cast<std::uint32_t>(keys);
    s.per_worker = s.keys / s.workers;
    return s;
}

// `form`, which is 0 or 1: true for the request/response primitive.
inline bool asks(const qvo::Params &p) {
    const long long v = p.get("form");
    if (v != 0 && v != 1) {
        std::fprintf(stderr, "savina/concdict: form must be 0 or 1 (got %lld)\n", v);
        std::abort();
    }
    return v == 1;
}

inline constexpr std::uint64_t kOpSeed     = 0xc0dc7d1c00000000ULL;
inline constexpr std::uint64_t kKeySeed    = 0xc0dc7d1c10000000ULL;
inline constexpr std::uint64_t kReplySeed  = 0xc0dc7d1c20000000ULL;
inline constexpr std::uint64_t kDoneSeed   = 0xc0dc7d1c30000000ULL;
inline constexpr std::uint64_t kEntrySeed  = 0xc0dc7d1c40000000ULL;
inline constexpr std::uint64_t kDigestSeed = 0xc0dc7d1c50000000ULL;

// Request j of worker w (both from 0): Savina's draw, made deterministic and PARTITIONED.
//
// Savina's worker draws from its own java.util.Random: `nextInt(100) < writePercent` decides a
// write, then a key (`|nextInt| % DATA_LIMIT`) and, for a write, a value. Here the same draws come
// from a mix of (w, j) -- the same distribution, a function an implementation cannot pick.
//
// THE DEVIATION: worker w's keys are the slots of ITS OWN partition, `slot * workers + w`, so no
// other worker ever writes a key w reads. In Savina all twenty workers share the key space, so
// what a read returns depends on whether another worker's write to that key landed first -- the
// scheduler's business, different run to run and framework to framework, and no checksum could
// assert it. Partitioned, every answer a worker receives is fixed by its OWN earlier requests
// (it has one in flight at a time), and so is the dictionary's final content. The dictionary is
// still one map holding all `keys` entries and serving every worker; a lookup costs what it costs
// in Savina, and every key a worker touches is one of Savina's.
struct Operation {
    std::uint32_t key;
    std::uint32_t value;  // the value a write stores; unused by a read
    bool          write;
};

inline Operation operation(const Shape &s, std::uint32_t worker, std::uint64_t j) noexcept {
    const std::uint64_t h    = qvo::mix(kOpSeed + (std::uint64_t{worker} << 32) + j);
    const std::uint64_t h2   = qvo::mix(h + kKeySeed);
    const auto          slot = static_cast<std::uint32_t>(h2 % s.per_worker);
    return Operation{slot * s.workers + worker, static_cast<std::uint32_t>(h2 >> 32),
                     h % 100 < s.write};
}

// The dictionary's content in the checksum: entry_key(k, v) for the value v under key k.
inline std::uint64_t entry_key(std::uint32_t key, std::uint32_t value) noexcept {
    return qvo::mix(kEntrySeed + (std::uint64_t{key} << 32) + value);
}

// The dictionary itself -- ONE class, compiled into every adapter, so the work a request does
// inside the dictionary actor is the same object code for every framework (FAIRNESS.md 1.3): a
// std::unordered_map lookup, and for a write an assignment. It is built before the framework is
// (outside the window, by the thread that runs body()) and handed to the dictionary actor.
//
// Beside the map it keeps `digest`, the wrapping sum over the keys written of
// entry_key(k, now) - entry_key(k, before): two mixes per write, which telescope to
// sum(entry_key(k, final) - entry_key(k, k)) -- a function of the final content alone, reported at
// the end without walking 524 287 entries. A write lost, applied to the wrong key or stored with
// the wrong value moves it.
class Store {
public:
    explicit Store(std::uint32_t keys) {
        _map.reserve(keys);
        for (std::uint32_t k = 0; k < keys; ++k) _map.emplace(k, k);  // Savina's DATA_MAP: k -> k
    }
    Store(const Store &)            = delete;
    Store &operator=(const Store &) = delete;

    std::uint32_t read(std::uint32_t key) const {
        const auto it = _map.find(key);
        if (it == _map.end()) fail("a read of a key outside the dictionary");
        return it->second;
    }

    // Savina's `dataMap.put(key, value)`, answered with the value stored.
    std::uint32_t write(std::uint32_t key, std::uint32_t value) {
        const auto it = _map.find(key);
        if (it == _map.end()) fail("a write of a key outside the dictionary");
        _digest += entry_key(key, value) - entry_key(key, it->second);
        it->second = value;
        return value;
    }

    std::uint64_t digest() const noexcept { return _digest; }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> _map;
    std::uint64_t                                    _digest{0};
};

// What a worker folds for the answer to its request j, and what the master folds for a worker's
// report and for the dictionary's. The request index is the worker's own count of the answers it
// has received, so an answer duplicated, lost, or carrying another request's value shifts or
// changes every term after it.
inline std::uint64_t reply_key(std::uint32_t worker, std::uint64_t j, std::uint32_t value) noexcept {
    return qvo::mix(value + qvo::mix(kReplySeed + (std::uint64_t{worker} << 32) + j));
}
inline std::uint64_t done_key(std::uint32_t worker, std::uint64_t fold) noexcept {
    return qvo::mix(fold + qvo::mix(kDoneSeed + worker));
}
inline std::uint64_t digest_key(std::uint64_t digest) noexcept {
    return qvo::mix(digest + kDigestSeed);
}

// The checksum. Worker w folds F(w) = sum_j reply_key(w, j, answer_j) and reports it in its Done;
// the master adds done_key(w, F(w)) for every Done and digest_key(D) for the dictionary's final
// digest D. Every interleaving gives the same total: worker w has one request in flight at a time,
// the dictionary serves requests one at a time, and nobody but w touches w's keys, so answer_j is
// the initial value of its key or w's own last write to it before j, and the final content of
// every key is w's last write to it. Computed here by replaying each worker's requests against its
// own partition, with no framework linked.
//
// A request lost or answered twice, an answer delivered to the wrong worker or carrying the wrong
// value, a write dropped or misapplied, a worker or the dictionary that never reported -- each moves
// the sum. A duplicate of a worker's LAST answer lands after its Done and moves nothing a checksum
// can see; a worker that receives it stops the run (fail()), when it is still there to receive it.
inline std::uint64_t expected(const qvo::Params &p) {
    const Shape s = shape(p);
    (void)asks(p);
    std::uint64_t acc    = 0;
    std::uint64_t digest = 0;
    for (std::uint32_t w = 0; w < s.workers; ++w) {
        std::unordered_map<std::uint32_t, std::uint32_t> own;  // w's writes: key -> last value
        std::uint64_t                                    fold = 0;
        for (std::uint64_t j = 0; j < s.messages; ++j) {
            const Operation o = operation(s, w, j);
            std::uint32_t   answer;
            if (o.write) {
                own[o.key] = o.value;
                answer     = o.value;
            } else {
                const auto it = own.find(o.key);
                answer        = it == own.end() ? o.key : it->second;
            }
            fold += reply_key(w, j, answer);
        }
        acc += done_key(w, fold);
        for (const auto &[k, v] : own) digest += entry_key(k, v) - entry_key(k, k);
    }
    return acc + digest_key(digest);
}

// The report divides by this: one operation -- a request to the dictionary and its answer.
inline constexpr const char *kWorkUnit = "operation";
inline std::uint64_t work_units(const qvo::Params &p) {
    const Shape s = shape(p);
    return std::uint64_t{s.workers} * s.messages;
}

// Counted at the receivers, the readiness handshake that opens the window excluded: per worker its
// Start and its `messages` answers, the dictionary's `messages` requests per worker and its Exit,
// the master's `workers` Dones and the dictionary's Report. With form=1 an ask is the same request
// and the same answer, so the count is the same. Each worker carries its count in its Done, the
// dictionary in its Report, and the master adds its own.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Shape s = shape(p);
    return 2 * std::uint64_t{s.workers} * s.messages + 2 * std::uint64_t{s.workers} + 2;
}

}  // namespace qvospec::savina::concdict

#endif  // QVOSPEC_SAVINA_CONCDICT_H
