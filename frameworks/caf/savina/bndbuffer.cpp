// @benchmark     savina/bndbuffer
// @framework     caf 1.1.0
// @idiom-source  the bank-transaction and fib adapters beside this file (function-based
//                behaviors, stateful_actor, built-in atoms typed per receiver, `self->spawn(...)`
//                from inside a behavior's init, `self->quit()` for an actor's own end) -- CAF's
//                own idioms from libcaf_core/caf/scheduled_actor.hpp and caf/event_based_mail.hpp.
// @idiom-note    Savina's actors one for one, as ProdConsAkkaActorBenchmark: the manager spawns
//                its producers and consumers and owns the bounded buffer as its own FIFO -- CAF's
//                mailbox is unbounded, and its backpressured flows (caf/flow, caf::async::
//                spsc_buffer) are a different shape (benchmarks/savina/bndbuffer.md), so the
//                buffer is the manager's protocol here as in every adapter. Messages are built-in
//                atoms plus arguments: ProduceData `(tick_atom, uint32 seq)`, DataItem
//                `(put_atom, uint32 producer, uint32 index, uint64 value)` in both of its legs,
//                ConsumerAvailable `(get_atom, uint32 consumer, uint64 delta, uint64 received)`,
//                ProducerExit `(leave_atom, uint32 producer, uint64 fold, uint64 received)`,
//                ConsumerExit `close_atom`, the readiness handshake `ok_atom`. A producer quits
//                after its exit message as the reference's does; the consumers quit on the
//                manager's close after the window. The work-stealing pool places everything.

#include <qvospec/savina/bndbuffer.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>

#include <utility>
#include <vector>

namespace savina_bndbuffer_caf {

using namespace qvospec::savina::bndbuffer;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t producer_waits{0};
    std::uint64_t consumer_waits{0};
    std::uint64_t buffer_peak{0};
};

struct producer_state {
    caf::actor    manager;
    std::uint32_t number{0};
    std::uint32_t items{0};
    int           iterations{1};
    std::uint64_t value{0};  // the chain (spec: produce)
    std::uint32_t produced{0};
    std::uint64_t fold{0};
    std::uint64_t received{0};
};

struct consumer_state {
    caf::actor    manager;
    std::uint32_t number{0};
    int           iterations{1};
    std::uint64_t received{0};
    std::uint64_t reported{0};
};

struct manager_state {
    std::vector<caf::actor>    producers;
    std::vector<caf::actor>    consumers;
    Counts                     counts{};
    std::uint64_t              threshold{0};
    Fifo<Item>                 buffer;
    Fifo<std::uint32_t>        available;  // consumers, oldest first
    Fifo<std::uint32_t>        parked;     // producers, oldest first
    std::vector<std::uint32_t> requests;   // ProduceData sent, per producer
    std::uint64_t              acc{0};     // the manager's terms of the checksum (bndbuffer.h)
    std::uint64_t              received{0};
    std::uint64_t              reported{0};  // messages counted by producers and consumers
    std::uint64_t              ready{0};
    std::uint32_t              ended{0};
    std::uint64_t              producer_waits{0};
    std::uint64_t              consumer_waits{0};
    std::uint64_t              peak{0};
    std::uint64_t              overflows{0};
    bool                       done{false};
    qvo::Watch                *watch{nullptr};
    Sink                      *sink{nullptr};
};

caf::behavior producer_fun(caf::stateful_actor<producer_state> *self, caf::actor manager,
                           std::uint32_t number, std::uint32_t items, int iterations) {
    auto &st      = self->state();
    st.manager    = std::move(manager);
    st.number     = number;
    st.items      = items;
    st.iterations = iterations;
    st.value      = producer_seed(number);
    self->mail(caf::ok_atom_v).send(st.manager);  // handshake, outside the window
    return {
        [self](caf::tick_atom, std::uint32_t seq) {  // ProduceData
            auto &s = self->state();
            ++s.received;
            s.fold += receipt(s.number, seq);
            if (s.produced == s.items) {  // asked once more after the last item: done
                self->mail(caf::leave_atom_v, s.number, s.fold, s.received).send(s.manager);
                self->quit();
                return;
            }
            s.value = produce(s.value, s.number, s.produced, s.iterations);
            self->mail(caf::put_atom_v, s.number, s.produced, s.value).send(s.manager);
            ++s.produced;
        },
    };
}

caf::behavior consumer_fun(caf::stateful_actor<consumer_state> *self, caf::actor manager,
                           std::uint32_t number, int iterations) {
    auto &st      = self->state();
    st.manager    = std::move(manager);
    st.number     = number;
    st.iterations = iterations;
    self->mail(caf::ok_atom_v).send(st.manager);  // handshake, outside the window
    return {
        [self](caf::put_atom, std::uint32_t producer, std::uint32_t index,
               std::uint64_t value) {  // DataItem
            auto &s = self->state();
            ++s.received;
            const std::uint64_t delta =
                item_term(producer, index, value, s.iterations) - route(s.number, producer, index);
            self->mail(caf::get_atom_v, s.number, delta, s.received - s.reported).send(s.manager);
            s.reported = s.received;
        },
        [self](caf::close_atom) { self->quit(); },  // ConsumerExit, after the window
    };
}

caf::behavior manager_fun(caf::stateful_actor<manager_state> *self, Counts counts, int prod_iters,
                          int cons_iters, qvo::Watch *watch, Sink *sink) {
    auto &st     = self->state();
    st.counts    = counts;
    st.threshold = threshold(counts);
    st.buffer    = Fifo<Item>(static_cast<std::size_t>(counts.buffer));
    st.available = Fifo<std::uint32_t>(counts.consumers);
    st.parked    = Fifo<std::uint32_t>(counts.producers);
    st.requests.assign(counts.producers, 0);
    st.watch = watch;
    st.sink  = sink;

    // The reference's manager creates its producers and consumers itself.
    const auto me = caf::actor_cast<caf::actor>(self);
    st.producers.reserve(counts.producers);
    for (std::uint32_t i = 0; i < counts.producers; ++i)
        st.producers.push_back(
            self->spawn<qvocaf::kSpawnOptions>(producer_fun, me, i, counts.items, prod_iters));
    st.consumers.reserve(counts.consumers);
    for (std::uint32_t j = 0; j < counts.consumers; ++j)
        st.consumers.push_back(self->spawn<qvocaf::kSpawnOptions>(consumer_fun, me, j, cons_iters));

    auto request = [self](std::uint32_t producer) {
        auto &s = self->state();
        self->mail(caf::tick_atom_v, ++s.requests[producer]).send(s.producers[producer]);
    };
    auto hand = [self](std::uint32_t consumer, const Item &item) {
        auto &s = self->state();
        s.acc += route(consumer, item.producer, item.index);
        self->mail(caf::put_atom_v, item.producer, item.index, item.value)
            .send(s.consumers[consumer]);
    };
    // The reference's tryExit: every producer done and every consumer available.
    auto try_exit = [self] {
        auto &s = self->state();
        if (s.done || s.ended != s.counts.producers || s.available.size() != s.counts.consumers)
            return;
        s.done = true;
        s.watch->stop();
        s.sink->checksum       = s.acc + overflow_weight() * s.overflows;
        s.sink->messages       = s.reported + s.received;
        s.sink->producer_waits = s.producer_waits;
        s.sink->consumer_waits = s.consumer_waits;
        s.sink->buffer_peak    = s.peak;
        for (auto &c : s.consumers) self->mail(caf::close_atom_v).send(c);
        self->quit();
    };

    return {
        // Every producer and consumer is up: every consumer available, every producer asked for
        // data -- the reference's onPostStart -- and the window open.
        [self, request](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != static_cast<std::uint64_t>(s.counts.producers) + s.counts.consumers)
                return;
            for (std::uint32_t c = 0; c < s.counts.consumers; ++c) s.available.push(c);
            s.watch->start();
            for (std::uint32_t p = 0; p < s.counts.producers; ++p) request(p);
        },
        [self, request, hand](caf::put_atom, std::uint32_t producer, std::uint32_t index,
                              std::uint64_t value) {  // DataItem
            auto &s = self->state();
            ++s.received;
            const Item item{producer, index, value};
            if (s.available.empty()) {
                s.buffer.push(item);
                if (s.buffer.size() > s.peak) s.peak = s.buffer.size();
                if (s.buffer.size() >= s.counts.buffer) ++s.overflows;  // the protocol cannot
            } else {
                hand(s.available.pop(), item);
            }
            if (s.buffer.size() >= s.threshold) {
                s.parked.push(producer);
                ++s.producer_waits;
            } else {
                request(producer);
            }
        },
        [self, request, hand, try_exit](caf::get_atom, std::uint32_t consumer, std::uint64_t delta,
                                        std::uint64_t received) {  // ConsumerAvailable
            auto &s = self->state();
            ++s.received;
            s.acc += delta;
            s.reported += received;
            if (s.buffer.empty()) {
                s.available.push(consumer);
                ++s.consumer_waits;
                try_exit();
            } else {
                hand(consumer, s.buffer.pop());
                if (!s.parked.empty()) request(s.parked.pop());
            }
        },
        [self, try_exit](caf::leave_atom, std::uint32_t, std::uint64_t fold,
                         std::uint64_t received) {  // ProducerExit
            auto &s = self->state();
            ++s.received;
            s.acc += fold;
            s.reported += received;
            ++s.ended;
            try_exit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Counts counts     = qvospec::savina::bndbuffer::counts(p);
    const int    prod_iters = iterations_of(p.get("prod_cost"));
    const int    cons_iters = iterations_of(p.get("cons_cost"));
    const auto   cores      = static_cast<std::size_t>(p.get("cores"));
    const bool   spin       = p.get("wait") != 0;
    qvocaf::refuse_spin_if_detached(spin);

    Sink sink;
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(manager_fun, counts, prod_iters, cons_iters, &watch,
                                         &sink);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedProducerWaits] = sink.producer_waits;
    answer.observed[kObservedConsumerWaits] = sink.consumer_waits;
    answer.observed[kObservedBufferPeak]    = sink.buffer_peak;
    return answer;
}

}  // namespace savina_bndbuffer_caf

int main(int argc, char **argv) {
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::bndbuffer::params();
    spec.expected          = qvospec::savina::bndbuffer::expected;
    spec.expected_messages = qvospec::savina::bndbuffer::expected_messages;
    spec.work_unit         = qvospec::savina::bndbuffer::kWorkUnit;
    spec.work_units        = qvospec::savina::bndbuffer::work_units;
    spec.idiom_source      = "the bank-transaction and fib adapters + self->spawn() from a "
                             "behavior's init (scheduled_actor.hpp) + mail().send() "
                             "(event_based_mail.hpp)";
    spec.idiom_note        = "Savina's manager, producers and consumers one for one, the manager "
                             "spawning the others; the bounded buffer is the manager's FIFO and "
                             "Savina's park/un-park protocol; built-in atoms typed per receiver; "
                             "placement left to the work-stealing pool";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the manager, the 40 producers and the 40 consumers are placed by the work-stealing pool; "
        "qb's cell pins the manager on core 0 and splits each kind evenly over the cores -- see "
        "benchmarks/savina/bndbuffer.md");

    return qvo::run(argc, argv, std::move(spec), savina_bndbuffer_caf::body);
}
