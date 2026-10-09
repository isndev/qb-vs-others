// savina/bndbuffer — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Producer-Consumer with Bounded Buffer (Imam & Sarkar, AGERE 2014), one of the
// "concurrency" benchmarks -- ProdConsBoundedBufferConfig.java and
// ProdConsAkkaActorBenchmark.scala. Deviations are recorded in benchmarks/savina/bndbuffer.md.

#ifndef QVOSPEC_SAVINA_BNDBUFFER_H
#define QVOSPEC_SAVINA_BNDBUFFER_H

#include <qvo/harness.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace qvospec::savina::bndbuffer {

inline constexpr const char *kId = "savina/bndbuffer";

// The shape, Savina's own: ONE manager owns a bounded buffer, `producers` producers each produce
// `items` data items and `consumers` consumers consume them; nobody talks to anybody but the
// manager. The manager starts by marking every consumer available and asking every producer for
// data (`ProduceData`). A producer asked for data busy-works one item and sends it to the manager
// (`DataItem`), then waits to be asked again; asked once more after its last item, it tells the
// manager it is done (`ProducerExit`) and ends. The manager hands an arriving item to the oldest
// available consumer or, with none available, appends it to its buffer; then, if the buffer holds
// `buffer - producers` items or more, it PARKS the producer (it does not ask it for more), and
// otherwise asks it at once. A consumer handed an item busy-works it and tells the manager it is
// available again (`ConsumerAvailable`); the manager gives it the oldest buffered item -- and then
// un-parks the oldest parked producer -- or, the buffer being empty, puts it back on the available
// list. Once every producer is done and every consumer is available, the run is over.
//
// That threshold is Savina's: `buffer - producers`, so that a producer that sent an item just
// before the threshold was reached still finds a slot. The buffer therefore NEVER holds more than
// `buffer - 1` items (the threshold minus one, plus one item from each producer before it is
// parked), which the manager asserts: the bound is a property of the protocol, and this is the
// benchmark that measures what keeping it costs (Huly QB-53). The buffer is the manager's explicit
// protocol in every implementation -- never a framework's mailbox bound.
//
// WHICH consumer gets which item, how often a producer is parked and how full the buffer gets
// depend on the interleaving and differ run to run and framework to framework. The checksum is
// built from what every interleaving must deliver (see expected()); the interleaving-dependent
// counts are reported beside the cell (qvo::Answer::observed), never asserted.
//
// `buffer`    -- the buffer's capacity. Savina's own default, 50; no deviation. Must exceed
//                `producers`: at `buffer <= producers` Savina's threshold is 0 or less, every
//                producer is parked after every item and only a buffered item un-parks one, so the
//                run stops forever on the first item handed straight to a consumer.
// `producers` -- Savina's own default, 40; no deviation.
// `consumers` -- Savina's own default, 40; no deviation.
// `items`     -- items per producer. Savina's own default, 1 000; no deviation: 40 000 items, each
//                produced, handed over and consumed once per repetition.
// `prod_cost` -- the producer's busy work per item. Savina's own default, 25; no deviation in the
//                parameter. Savina's processItem(cost) runs `cost x 100` iterations of one
//                `nextDouble()` and one `Math.log()` (one iteration when cost <= 0); here each
//                iteration is one step of qvo::spin_work, the harness's busy work -- the same
//                mapping savina/barber makes for one `Math.random()` call.
// `cons_cost` -- the consumer's busy work per item; as `prod_cost`. Savina's own default, 25.
// `cores`     -- 1: everything on one thread; 2: for the frameworks that place, the manager on
//                core 0, producer i on core (1 + i) % cores and consumer j on core
//                (1 + producers + j) % cores -- half of each kind on each core, so the busy work
//                splits evenly whatever the two costs are, and about half the hand-offs cross.
// `wait`      -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
inline std::map<std::string, long long> params() {
    return {{"buffer", 50},    {"producers", 40}, {"consumers", 40}, {"items", 1000},
            {"prod_cost", 25}, {"cons_cost", 25}, {"cores", 2},      {"wait", 1}};
}

// The names under which every implementation reports what the interleaving decided
// (qvo::Answer::observed). Reported, not asserted.
inline constexpr const char *kObservedProducerWaits = "producer_waits";  // producers parked
inline constexpr const char *kObservedConsumerWaits = "consumer_waits";  // consumers idled
inline constexpr const char *kObservedBufferPeak    = "buffer_peak";     // most items buffered

// A parameter outside its domain stops the run before anything is built.
[[noreturn]] inline void refuse(const char *what, long long value) {
    std::fprintf(stderr, "savina/bndbuffer: %s (got %lld)\n", what, value);
    std::abort();
}

// processItem(cost): `cost x 100` iterations, one when cost <= 0 (Savina's else-branch).
inline int iterations_of(long long cost) {
    if (cost > 20000000LL) refuse("a cost above 20 000 000 overflows the iteration count", cost);
    return cost > 0 ? static_cast<int>(cost * 100) : 1;
}

// The four counts, validated: every one at least 1, `buffer` above `producers`, and producer and
// item numbers small enough for the 32-bit fields the messages carry (a request's sequence number
// goes up to `items + 1`).
struct Counts {
    std::uint64_t buffer;
    std::uint32_t producers;
    std::uint32_t consumers;
    std::uint32_t items;
};

inline Counts counts(const qvo::Params &p) {
    const long long     buffer    = p.get("buffer");
    const long long     producers = p.get("producers");
    const long long     consumers = p.get("consumers");
    const long long     items     = p.get("items");
    constexpr long long kMax31    = 0x7fffffffLL;
    if (producers < 1 || producers > kMax31) refuse("producers must be in [1, 2^31)", producers);
    if (consumers < 1 || consumers > kMax31) refuse("consumers must be in [1, 2^31)", consumers);
    if (items < 1 || items > kMax31) refuse("items must be in [1, 2^31)", items);
    if (buffer <= producers)
        refuse("buffer must exceed producers (Savina's threshold is buffer - producers)", buffer);
    (void)iterations_of(p.get("prod_cost"));
    (void)iterations_of(p.get("cons_cost"));
    return Counts{static_cast<std::uint64_t>(buffer), static_cast<std::uint32_t>(producers),
                  static_cast<std::uint32_t>(consumers), static_cast<std::uint32_t>(items)};
}

// Savina's threshold: at this many buffered items or more, the producer that just delivered is
// parked instead of being asked for its next item. At least 1, since buffer > producers.
inline std::uint64_t threshold(const Counts &c) noexcept { return c.buffer - c.producers; }

// ---------------------------------------------------------------------------------------------
// The work, and what each item is
// ---------------------------------------------------------------------------------------------

inline constexpr std::uint64_t kProducerSeed = 0xb0bbe7f000000001ULL;
inline constexpr std::uint64_t kConsumeSeed  = 0xb0bbe7f000000002ULL;
inline constexpr std::uint64_t kItemTag      = 0xb0bbe7f000000003ULL;
inline constexpr std::uint64_t kProducerTag  = 0xb0bbe7f000000004ULL;
inline constexpr std::uint64_t kConsumerTag  = 0xb0bbe7f000000005ULL;
inline constexpr std::uint64_t kProduceTag   = 0xb0bbe7f000000006ULL;
inline constexpr std::uint64_t kOverflowTag  = 0xb0bbe7f000000007ULL;

// Item k of producer p (both from 0), as one 64-bit key.
inline std::uint64_t key(std::uint32_t producer, std::uint32_t index) noexcept {
    return (static_cast<std::uint64_t>(producer) << 32) | index;
}

// A producer's value before its first item. Like Savina's `prodItem`, a producer's value is a
// CHAIN: each item is computed from the previous one, so a producer works in order and an item
// cannot be produced without the ones before it.
inline std::uint64_t producer_seed(std::uint32_t producer) noexcept {
    return qvo::mix(kProducerSeed + producer);
}

// The producer's busy work for its item `index`: the value it sends.
inline std::uint64_t produce(std::uint64_t previous, std::uint32_t producer, std::uint32_t index,
                             int iterations) noexcept {
    return qvo::spin_work(qvo::mix(previous + key(producer, index)), iterations);
}

// The consumer's busy work on a value. Savina's consumer also chains (`consItem =
// processItem(consItem + data)`), which is order-free there only because processItem is a sum;
// a mix chain is not, and which consumer gets which item is the scheduler's -- so each item is
// worked on its own, with the same amount of work (benchmarks/savina/bndbuffer.md, deviations).
inline std::uint64_t consume(std::uint64_t value, int iterations) noexcept {
    return qvo::spin_work(value ^ kConsumeSeed, iterations);
}

// ---------------------------------------------------------------------------------------------
// The checksum's terms
// ---------------------------------------------------------------------------------------------

inline std::uint64_t item_identity(std::uint32_t producer, std::uint32_t index) noexcept {
    return qvo::mix(kItemTag + key(producer, index));
}
inline std::uint64_t producer_identity(std::uint32_t producer) noexcept {
    return qvo::mix(kProducerTag + producer);
}
inline std::uint64_t consumer_identity(std::uint32_t consumer) noexcept {
    return qvo::mix(kConsumerTag + consumer);
}

// What consuming item (producer, index) of value `value` contributes, whoever consumes it.
inline std::uint64_t item_term(std::uint32_t producer, std::uint32_t index, std::uint64_t value,
                               int iterations) noexcept {
    return item_identity(producer, index) + consume(value, iterations);
}

// The routing receipt of one hand-over: the manager ADDS route(c, item) when it hands the item to
// consumer c, the consumer that receives it SUBTRACTS route(itself, item). The two cancel exactly
// when the item reached the consumer it was handed to, so a hand-over delivered to another
// consumer leaves a residual in the sum -- the one misroute the item terms alone cannot see, since
// any consumer may consume any item.
inline std::uint64_t route(std::uint32_t consumer, std::uint32_t producer,
                           std::uint32_t index) noexcept {
    return consumer_identity(consumer) * item_identity(producer, index);
}

// The receipt of a ProduceData. The manager numbers the requests it sends each producer from 1
// (`seq`), and the producer adds receipt(itself, seq) for every one it receives. A producer
// receives exactly `items + 1` of them -- one per item, and the one that ends it -- so every
// interleaving gives the same sum, while a duplicated request moves it: the producer receives one
// sequence number twice and ends on its `items + 1`-th receipt, so the last number never reaches
// its sum (a duplicate of that LAST request is the one exception: it finds the producer already
// ended and changes nothing the producer did). A request delivered to another producer moves both
// sums -- and starves the producer it was meant for, so that run never ends: a hang, not a number.
inline std::uint64_t receipt(std::uint32_t producer, std::uint32_t seq) noexcept {
    return producer_identity(producer) * qvo::mix(kProduceTag + seq);
}

// What the manager adds for every time an append left the buffer holding `buffer` items or more --
// which Savina's protocol can never do (the header comment). Never zero, so a run that broke the
// bound cannot verify.
inline std::uint64_t overflow_weight() noexcept { return qvo::mix(kOverflowTag); }

// The checksum:
//
//     sum over items (p, k) of   item_identity(p, k) + consume(v(p, k))
//   + sum over producers p of    sum over seq = 1 .. items + 1 of receipt(p, seq)
//
// where v(p, k) is producer p's chain. The consumer that consumes item (p, k) adds its term minus
// route(itself, p, k) and the manager adds route(c, p, k) for the consumer c it handed it to
// (they cancel); each producer reports its receipts when it ends. Every interleaving gives the same
// total -- which consumer took which item only moves terms that cancel -- and a dropped item, a
// doubled one, an item produced without its chain, a hand-over delivered to the wrong consumer,
// a doubled ProduceData or an overflowed buffer moves it.
inline std::uint64_t expected(const qvo::Params &p) {
    const Counts  c     = counts(p);
    const int     prod  = iterations_of(p.get("prod_cost"));
    const int     cons  = iterations_of(p.get("cons_cost"));
    std::uint64_t acc   = 0;
    for (std::uint32_t producer = 0; producer < c.producers; ++producer) {
        std::uint64_t v = producer_seed(producer);
        for (std::uint32_t k = 0; k < c.items; ++k) {
            v = produce(v, producer, k, prod);
            acc += item_term(producer, k, v, cons);
        }
        for (std::uint32_t seq = 1; seq <= c.items + 1; ++seq) acc += receipt(producer, seq);
    }
    return acc;
}

// The report divides by this: one item -- produced, handed over (directly or through the buffer)
// and consumed.
inline constexpr const char *kWorkUnit = "item";
inline std::uint64_t work_units(const qvo::Params &p) {
    const Counts c = counts(p);
    return static_cast<std::uint64_t>(c.producers) * c.items;
}

// Inside the window, counted at the receivers: every producer receives `items + 1` ProduceData;
// the manager receives `items` DataItems from each producer, one ConsumerAvailable per item and
// one ProducerExit per producer; the consumers receive one DataItem per item. The interleaving
// decides who, never how many: 4 x producers x items + 2 x producers. The readiness handshake
// before the window and the end of the consumers after it are not counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Counts        c = counts(p);
    const std::uint64_t n = static_cast<std::uint64_t>(c.producers) * c.items;
    return 4 * n + 2 * static_cast<std::uint64_t>(c.producers);
}

// ---------------------------------------------------------------------------------------------
// The manager's lists -- one implementation for every adapter
// ---------------------------------------------------------------------------------------------

// A buffered item, as the manager keeps it.
struct Item {
    std::uint32_t producer{0};
    std::uint32_t index{0};
    std::uint64_t value{0};
};

// The manager's three FIFO lists (the buffer, the available consumers, the parked producers):
// Savina keeps ListBuffers. A power-of-two ring sized to the protocol's bound, so a correct run
// never allocates inside the window, and that grows by doubling past it rather than fail -- only a
// run that broke the protocol can need it, and the checksum reports that run. One implementation
// here, so the container is the same object code in every framework's manager and never a
// difference between them (a std::deque would not be: MSVC's allocates every few elements).
template <typename T>
class Fifo {
public:
    Fifo() : Fifo(16) {}
    explicit Fifo(std::size_t capacity) : slots_(round_up(capacity)) {}

    bool        empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    void push(const T &value) {
        if (size_ == slots_.size()) grow();
        slots_[(head_ + size_) & (slots_.size() - 1)] = value;
        ++size_;
    }

    // Precondition: !empty().
    T pop() noexcept {
        const T value = slots_[head_];
        head_         = (head_ + 1) & (slots_.size() - 1);
        --size_;
        return value;
    }

private:
    static std::size_t round_up(std::size_t n) {
        std::size_t c = 1;
        while (c < n) c <<= 1;
        return c;
    }

    void grow() {
        std::vector<T> next(slots_.size() * 2);
        for (std::size_t i = 0; i < size_; ++i) next[i] = slots_[(head_ + i) & (slots_.size() - 1)];
        slots_.swap(next);
        head_ = 0;
    }

    std::vector<T> slots_;
    std::size_t    head_{0};
    std::size_t    size_{0};
};

}  // namespace qvospec::savina::bndbuffer

#endif  // QVOSPEC_SAVINA_BNDBUFFER_H
