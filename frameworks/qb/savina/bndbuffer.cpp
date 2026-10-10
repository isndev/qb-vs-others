// @benchmark     savina/bndbuffer
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md -- "`send<T>()` is unordered; `push<T>()` is ordered" (`send`
//                hands the event to the peer's ring at once instead of the pass's batched flush)
//                and the Actor section (`registerEvent`, `kill()` for an actor's own end,
//                `broadcast<qb::KillEvent>()` to end the run) -- and the bank-transaction adapter
//                beside this file (a Ready handshake before the window, placement fixed by
//                `addActor(core, ...)`).
// @idiom-note    Savina's actors one for one -- a manager, `producers` producers, `consumers`
//                consumers -- and Savina's protocol, the buffer being the manager's own FIFO: qb
//                has no bounded mailbox and no credit primitive between actors (`push` and `send`
//                always succeed; see benchmarks/savina/bndbuffer.md), so a bounded buffer IS this
//                protocol, in qb as in the reference. Every hand-over is `send<>`, never `push`:
//                each actor has at most one message in flight to or from the manager, so no
//                order is ever at stake, and a core here spends most of a pass inside 2 500-step
//                busy-work handlers -- a `push` would wait for the end of that pass to be
//                flushed while the other core runs dry, where `send` lands in the peer's ring at
//                once (on the same core it is the same local queue a `push` uses). A producer
//                `kill()`s itself after its exit message, as the reference's does; the consumers
//                and the manager end with the engine-wide KillEvent after the window. The manager
//                is on VirtualCore 0, producer i on core (1 + i) % cores and consumer j on core
//                (1 + producers + j) % cores.

#include <qvospec/savina/bndbuffer.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_bndbuffer_qb {

using namespace qvospec::savina::bndbuffer;

// Savina's message set, one type each. Data is the reference's DataItemMessage in both of its
// legs (producer -> manager, manager -> consumer), Available its ConsumerAvailableMessage, Done
// its ProducerExitMessage. The consumers' exit message is the engine's KillEvent, after the
// window.
struct Ready : qb::Event {};  // handshake, outside the window
struct Produce : qb::Event {
    std::uint32_t seq;  // the manager's count of the requests it sent this producer (spec: receipt)
    explicit Produce(std::uint32_t s) noexcept : seq(s) {}
};
struct Data : qb::Event {
    std::uint32_t producer;
    std::uint32_t index;
    std::uint64_t value;
    Data(std::uint32_t p, std::uint32_t i, std::uint64_t v) noexcept
        : producer(p), index(i), value(v) {}
};
struct Available : qb::Event {
    std::uint32_t consumer;
    std::uint64_t delta;     // the item's term minus this consumer's routing receipt
    std::uint64_t received;  // messages this consumer received since its last Available
    Available(std::uint32_t c, std::uint64_t d, std::uint64_t r) noexcept
        : consumer(c), delta(d), received(r) {}
};
struct Done : qb::Event {
    std::uint32_t producer;
    std::uint64_t fold;      // the producer's receipts
    std::uint64_t received;  // the requests it received
    Done(std::uint32_t p, std::uint64_t f, std::uint64_t r) noexcept
        : producer(p), fold(f), received(r) {}
};

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t producer_waits{0};
    std::uint64_t consumer_waits{0};
    std::uint64_t buffer_peak{0};
};

// The ids, filled before the engine starts (ids are assigned at addActor time, the actors are
// constructed at start) and read-only from then on.
struct Field {
    qb::ActorId              manager;
    std::vector<qb::ActorId> producers;
    std::vector<qb::ActorId> consumers;
};

class Producer final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _number;
    const std::uint32_t _items;
    const int           _iterations;
    std::uint64_t       _value;  // the chain (spec: produce)
    std::uint32_t       _produced{0};
    std::uint64_t       _fold{0};
    std::uint64_t       _received{0};

public:
    Producer(const Field &field, std::uint32_t number, std::uint32_t items, int iterations) noexcept
        : _field(field)
        , _number(number)
        , _items(items)
        , _iterations(iterations)
        , _value(producer_seed(number)) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Produce>(*this);
        push<Ready>(_field.manager);
        co_return true;
    }

    void on(Produce const &event) {
        ++_received;
        _fold += receipt(_number, event.seq);
        if (_produced == _items) {  // asked once more after the last item: done, as the reference
            send<Done>(_field.manager, _number, _fold, _received);
            kill();
            return;
        }
        _value = produce(_value, _number, _produced, _iterations);
        send<Data>(_field.manager, _number, _produced, _value);
        ++_produced;
    }
};

class Consumer final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _number;
    const int           _iterations;
    std::uint64_t       _received{0};
    std::uint64_t       _reported{0};

public:
    Consumer(const Field &field, std::uint32_t number, int iterations) noexcept
        : _field(field), _number(number), _iterations(iterations) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Data>(*this);
        push<Ready>(_field.manager);
        co_return true;
    }

    void on(Data const &event) {
        ++_received;
        const std::uint64_t delta =
            item_term(event.producer, event.index, event.value, _iterations) -
            route(_number, event.producer, event.index);
        send<Available>(_field.manager, _number, delta, _received - _reported);
        _reported = _received;
    }
};

class Manager final : public qb::Actor {
    const Field               &_field;
    const Counts               _counts;
    const std::uint64_t        _threshold;
    qvo::Watch                &_watch;
    Sink                      &_sink;
    Fifo<Item>                 _buffer;
    Fifo<std::uint32_t>        _available;  // consumers, oldest first
    Fifo<std::uint32_t>        _parked;     // producers, oldest first
    std::vector<std::uint32_t> _requests;   // ProduceData sent, per producer
    std::uint64_t              _acc{0};     // the manager's terms of the checksum (bndbuffer.h)
    std::uint64_t              _received{0};
    std::uint64_t              _reported{0};  // messages counted by producers and consumers
    std::uint64_t              _ready{0};
    std::uint32_t              _ended{0};
    std::uint64_t              _producer_waits{0};
    std::uint64_t              _consumer_waits{0};
    std::uint64_t              _peak{0};
    std::uint64_t              _overflows{0};
    bool                       _done{false};

    void request(std::uint32_t producer) {
        send<Produce>(_field.producers[producer], ++_requests[producer]);
    }

    void hand(std::uint32_t consumer, const Item &item) {
        _acc += route(consumer, item.producer, item.index);
        send<Data>(_field.consumers[consumer], item.producer, item.index, item.value);
    }

    // The reference's tryExit: every producer done and every consumer available. At-least tests,
    // identical to the reference's on a correct run: a run that duplicated a message can step past
    // either count, and it must end with its wrong checksum rather than wait forever.
    void try_exit() {
        if (_done || _ended < _counts.producers || _available.size() < _counts.consumers) return;
        _done = true;
        _watch.stop();
        _sink.checksum       = _acc + overflow_weight() * _overflows;
        _sink.messages       = _reported + _received;
        _sink.producer_waits = _producer_waits;
        _sink.consumer_waits = _consumer_waits;
        _sink.buffer_peak    = _peak;
        broadcast<qb::KillEvent>();
    }

public:
    Manager(const Field &field, Counts counts, qvo::Watch &watch, Sink &sink)
        : _field(field)
        , _counts(counts)
        , _threshold(threshold(counts))
        , _watch(watch)
        , _sink(sink)
        , _buffer(static_cast<std::size_t>(counts.buffer))
        , _available(counts.consumers)
        , _parked(counts.producers)
        , _requests(counts.producers, 0) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Data>(*this);
        registerEvent<Available>(*this);
        registerEvent<Done>(*this);
        co_return true;
    }

    // Every producer and consumer is up and scheduled: every consumer available, every producer
    // asked for data -- the reference's onPostStart -- and the window open.
    void on(Ready const &) {
        if (++_ready != static_cast<std::uint64_t>(_counts.producers) + _counts.consumers) return;
        for (std::uint32_t c = 0; c < _counts.consumers; ++c) _available.push(c);
        _watch.start();
        for (std::uint32_t p = 0; p < _counts.producers; ++p) request(p);
    }

    void on(Data const &event) {
        ++_received;
        const Item item{event.producer, event.index, event.value};
        if (_available.empty()) {
            _buffer.push(item);
            if (_buffer.size() > _peak) _peak = _buffer.size();
            if (_buffer.size() >= _counts.buffer) ++_overflows;  // the protocol cannot: asserted
        } else {
            hand(_available.pop(), item);
        }
        if (_buffer.size() >= _threshold) {
            _parked.push(event.producer);
            ++_producer_waits;
        } else {
            request(event.producer);
        }
        if (!bound_holds(_buffer.size(), _parked.size(), _ended, _counts)) ++_overflows;
    }

    void on(Available const &event) {
        ++_received;
        _acc += event.delta;
        _reported += event.received;
        if (_buffer.empty()) {
            _available.push(event.consumer);
            ++_consumer_waits;
            try_exit();
        } else {
            hand(event.consumer, _buffer.pop());
            if (!_parked.empty()) request(_parked.pop());
        }
    }

    void on(Done const &event) {
        ++_received;
        _acc += event.fold;
        _reported += event.received;
        ++_ended;
        try_exit();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Counts counts     = qvospec::savina::bndbuffer::counts(p);
    const int    prod_iters = iterations_of(p.get("prod_cost"));
    const int    cons_iters = iterations_of(p.get("cons_cost"));
    const auto   cores      = static_cast<int>(p.get("cores"));
    const bool   spin       = p.get("wait") != 0;

    Sink  sink;
    Field field;
    {
        qb::Main engine;

        const int ncores = cores < 1 ? 1 : cores;
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        // The manager on core 0; producer i on core (1 + i) % cores, consumer j on core
        // (1 + producers + j) % cores: half of each kind on each core at cores=2.
        field.manager = engine.addActor<Manager>(0, std::cref(field), counts, std::ref(watch),
                                                 std::ref(sink));
        field.producers.reserve(counts.producers);
        for (std::uint32_t i = 0; i < counts.producers; ++i) {
            const auto core = static_cast<qb::CoreId>((1 + static_cast<long long>(i)) % ncores);
            field.producers.push_back(engine.addActor<Producer>(core, std::cref(field), i,
                                                                counts.items, prod_iters));
        }
        field.consumers.reserve(counts.consumers);
        for (std::uint32_t j = 0; j < counts.consumers; ++j) {
            const auto core = static_cast<qb::CoreId>(
                (1 + static_cast<long long>(counts.producers) + j) % ncores);
            field.consumers.push_back(
                engine.addActor<Consumer>(core, std::cref(field), j, cons_iters));
        }

        engine.start();
        engine.join();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedProducerWaits] = sink.producer_waits;
    answer.observed[kObservedConsumerWaits] = sink.consumer_waits;
    answer.observed[kObservedBufferPeak]    = sink.buffer_peak;
    return answer;
}

}  // namespace savina_bndbuffer_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bndbuffer::params();
    spec.expected          = qvospec::savina::bndbuffer::expected;
    spec.expected_messages = qvospec::savina::bndbuffer::expected_messages;
    spec.work_unit         = qvospec::savina::bndbuffer::kWorkUnit;
    spec.work_units        = qvospec::savina::bndbuffer::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: send<T>() (into the peer's ring at once, not at "
                             "the pass's flush) + kill() + broadcast<qb::KillEvent>(); the "
                             "bank-transaction adapter's Ready handshake and fixed placement";
    spec.idiom_note        = "Savina's manager, producers and consumers one for one; the bounded "
                             "buffer is the manager's FIFO and Savina's park/un-park protocol (qb "
                             "has no bounded mailbox or credit primitive between actors); every "
                             "hand-over is send<>; manager on VirtualCore 0, producer i on core "
                             "(1 + i) % cores, consumer j on (1 + producers + j) % cores";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "every hand-over is send<>, which publishes a cross-core event into the peer's ring at "
        "once; a push would be published by the pass's flush, after every busy-work handler the "
        "pass runs -- each actor has at most one message in flight to or from the manager, so "
        "the unordered primitive changes no outcome");
    spec.caveats.emplace_back(
        "placement is fixed before start: the manager on VirtualCore 0, producer i on core "
        "(1 + i) % cores, consumer j on core (1 + producers + j) % cores, so with cores=2 half of "
        "each kind shares the manager's core and about half the hand-overs cross a core; the "
        "manager answers only between the busy-work handlers of its own core -- the pools place "
        "freely");

    return qvo::run(argc, argv, std::move(spec), savina_bndbuffer_qb::body);
}
