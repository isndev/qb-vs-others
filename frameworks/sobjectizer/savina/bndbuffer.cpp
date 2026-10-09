// @benchmark     savina/bndbuffer
// @framework     sobjectizer 5.8.5.1
// @idiom-source  the bank-transaction adapter beside this file (agent_t subclasses on
//                their DIRECT mboxes, one coop bound to qvoso::make_pool_binder, readiness sent
//                from so_evt_start) -- SObjectizer's own idioms from dev/so_5/agent.hpp and
//                dev/so_5/send_functions.hpp.
// @idiom-note    Savina's actors one for one, as agents on their direct mboxes, in one coop on
//                the thread pool with individual FIFOs so they run concurrently. The bounded
//                buffer is the manager's own FIFO and Savina's park/un-park protocol: an agent's
//                message limits (limit_then_drop and its siblings, dev/so_5/message_limit.hpp)
//                drop, redirect or abort past a bound rather than hold a producer back, and a
//                size-limited mchain that makes `send` WAIT blocks the sending work thread -- the
//                one a consumer on the same pool may need to drain it -- so neither is this
//                shape (benchmarks/savina/bndbuffer.md). A producer ends by going silent after
//                its exit message: SObjectizer ends an agent only with its whole coop, and the
//                coop ends when the manager stops the environment after the window.

#include <qvospec/savina/bndbuffer.h>

#include "../so_support.h"

#include <so_5/all.hpp>

#include <vector>

namespace savina_bndbuffer_sobjectizer {

using namespace qvospec::savina::bndbuffer;

struct Sink {
    std::uint64_t checksum{0};
    std::uint64_t messages{0};
    std::uint64_t producer_waits{0};
    std::uint64_t consumer_waits{0};
    std::uint64_t buffer_peak{0};
};

struct msg_ready final : public so_5::signal_t {};  // handshake, outside the window
struct msg_produce final : public so_5::message_t {  // ProduceData
    std::uint32_t seq;
    explicit msg_produce(std::uint32_t s) noexcept : seq(s) {}
};
struct msg_data final : public so_5::message_t {  // DataItem, both legs
    std::uint32_t producer;
    std::uint32_t index;
    std::uint64_t value;
    msg_data(std::uint32_t p, std::uint32_t i, std::uint64_t v) noexcept
        : producer(p), index(i), value(v) {}
};
struct msg_available final : public so_5::message_t {  // ConsumerAvailable
    std::uint32_t consumer;
    std::uint64_t delta;
    std::uint64_t received;
    msg_available(std::uint32_t c, std::uint64_t d, std::uint64_t r) noexcept
        : consumer(c), delta(d), received(r) {}
};
struct msg_done final : public so_5::message_t {  // ProducerExit
    std::uint32_t producer;
    std::uint64_t fold;
    std::uint64_t received;
    msg_done(std::uint32_t p, std::uint64_t f, std::uint64_t r) noexcept
        : producer(p), fold(f), received(r) {}
};

class producer_t final : public so_5::agent_t {
    const so_5::mbox_t  m_manager;
    const std::uint32_t m_number;
    const std::uint32_t m_items;
    const int           m_iterations;
    std::uint64_t       m_value;  // the chain (spec: produce)
    std::uint32_t       m_produced{0};
    std::uint64_t       m_fold{0};
    std::uint64_t       m_received{0};

public:
    producer_t(context_t ctx, so_5::mbox_t manager, std::uint32_t number, std::uint32_t items,
               int iterations)
        : so_5::agent_t{std::move(ctx)}
        , m_manager{std::move(manager)}
        , m_number{number}
        , m_items{items}
        , m_iterations{iterations}
        , m_value{producer_seed(number)} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_produce> m) {
            ++m_received;
            m_fold += receipt(m_number, m->seq);
            if (m_produced == m_items) {  // asked once more after the last item: done
                so_5::send<msg_done>(m_manager, m_number, m_fold, m_received);
                return;
            }
            m_value = produce(m_value, m_number, m_produced, m_iterations);
            so_5::send<msg_data>(m_manager, m_number, m_produced, m_value);
            ++m_produced;
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_manager); }
};

class consumer_t final : public so_5::agent_t {
    const so_5::mbox_t  m_manager;
    const std::uint32_t m_number;
    const int           m_iterations;
    std::uint64_t       m_received{0};
    std::uint64_t       m_reported{0};

public:
    consumer_t(context_t ctx, so_5::mbox_t manager, std::uint32_t number, int iterations)
        : so_5::agent_t{std::move(ctx)}
        , m_manager{std::move(manager)}
        , m_number{number}
        , m_iterations{iterations} {}

    void so_define_agent() override {
        so_subscribe_self().event([this](so_5::mhood_t<msg_data> m) {
            ++m_received;
            const std::uint64_t delta = item_term(m->producer, m->index, m->value, m_iterations) -
                                        route(m_number, m->producer, m->index);
            so_5::send<msg_available>(m_manager, m_number, delta, m_received - m_reported);
            m_reported = m_received;
        });
    }

    void so_evt_start() override { so_5::send<msg_ready>(m_manager); }
};

class manager_t final : public so_5::agent_t {
    const Counts               m_counts;
    const std::uint64_t        m_threshold;
    qvo::Watch                &m_watch;
    Sink                      &m_sink;
    std::vector<so_5::mbox_t>  m_producers;
    std::vector<so_5::mbox_t>  m_consumers;
    Fifo<Item>                 m_buffer;
    Fifo<std::uint32_t>        m_available;  // consumers, oldest first
    Fifo<std::uint32_t>        m_parked;     // producers, oldest first
    std::vector<std::uint32_t> m_requests;   // ProduceData sent, per producer
    std::uint64_t              m_acc{0};     // the manager's terms of the checksum (bndbuffer.h)
    std::uint64_t              m_received{0};
    std::uint64_t              m_reported{0};  // messages counted by producers and consumers
    std::uint64_t              m_ready{0};
    std::uint32_t              m_ended{0};
    std::uint64_t              m_producer_waits{0};
    std::uint64_t              m_consumer_waits{0};
    std::uint64_t              m_peak{0};
    std::uint64_t              m_overflows{0};
    bool                       m_done{false};

    void request(std::uint32_t producer) {
        so_5::send<msg_produce>(m_producers[producer], ++m_requests[producer]);
    }

    void hand(std::uint32_t consumer, const Item &item) {
        m_acc += route(consumer, item.producer, item.index);
        so_5::send<msg_data>(m_consumers[consumer], item.producer, item.index, item.value);
    }

    // The reference's tryExit: every producer done and every consumer available.
    void try_exit() {
        if (m_done || m_ended != m_counts.producers || m_available.size() != m_counts.consumers)
            return;
        m_done = true;
        m_watch.stop();
        m_sink.checksum       = m_acc + overflow_weight() * m_overflows;
        m_sink.messages       = m_reported + m_received;
        m_sink.producer_waits = m_producer_waits;
        m_sink.consumer_waits = m_consumer_waits;
        m_sink.buffer_peak    = m_peak;
        so_environment().stop();
    }

public:
    manager_t(context_t ctx, Counts counts, qvo::Watch &watch, Sink &sink)
        : so_5::agent_t{std::move(ctx)}
        , m_counts{counts}
        , m_threshold{threshold(counts)}
        , m_watch{watch}
        , m_sink{sink}
        , m_buffer(static_cast<std::size_t>(counts.buffer))
        , m_available(counts.consumers)
        , m_parked(counts.producers)
        , m_requests(counts.producers, 0) {}

    void wire(std::vector<so_5::mbox_t> producers, std::vector<so_5::mbox_t> consumers) {
        m_producers = std::move(producers);
        m_consumers = std::move(consumers);
    }

    void so_define_agent() override {
        so_subscribe_self()
            // Every producer and consumer is up: every consumer available, every producer asked
            // for data -- the reference's onPostStart -- and the window open.
            .event([this](so_5::mhood_t<msg_ready>) {
                if (++m_ready !=
                    static_cast<std::uint64_t>(m_counts.producers) + m_counts.consumers)
                    return;
                for (std::uint32_t c = 0; c < m_counts.consumers; ++c) m_available.push(c);
                m_watch.start();
                for (std::uint32_t p = 0; p < m_counts.producers; ++p) request(p);
            })
            .event([this](so_5::mhood_t<msg_data> m) {
                ++m_received;
                const Item item{m->producer, m->index, m->value};
                if (m_available.empty()) {
                    m_buffer.push(item);
                    if (m_buffer.size() > m_peak) m_peak = m_buffer.size();
                    if (m_buffer.size() >= m_counts.buffer) ++m_overflows;  // the protocol cannot
                } else {
                    hand(m_available.pop(), item);
                }
                if (m_buffer.size() >= m_threshold) {
                    m_parked.push(m->producer);
                    ++m_producer_waits;
                } else {
                    request(m->producer);
                }
            })
            .event([this](so_5::mhood_t<msg_available> m) {
                ++m_received;
                m_acc += m->delta;
                m_reported += m->received;
                if (m_buffer.empty()) {
                    m_available.push(m->consumer);
                    ++m_consumer_waits;
                    try_exit();
                } else {
                    hand(m->consumer, m_buffer.pop());
                    if (!m_parked.empty()) request(m_parked.pop());
                }
            })
            .event([this](so_5::mhood_t<msg_done> m) {
                ++m_received;
                m_acc += m->fold;
                m_reported += m->received;
                ++m_ended;
                try_exit();
            });
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Counts counts     = qvospec::savina::bndbuffer::counts(p);
    const int    prod_iters = iterations_of(p.get("prod_cost"));
    const int    cons_iters = iterations_of(p.get("cons_cost"));
    const auto   cores      = static_cast<int>(p.get("cores"));
    const bool   spin       = p.get("wait") != 0;

    Sink sink;
    so_5::launch([&](so_5::environment_t &env) {
        auto binder = qvoso::make_pool_binder(env, cores, spin);
        env.introduce_coop(binder, [&](so_5::coop_t &coop) {
            auto *manager = coop.make_agent<manager_t>(counts, std::ref(watch), std::ref(sink));
            const so_5::mbox_t        to_manager = manager->so_direct_mbox();
            std::vector<so_5::mbox_t> producers;
            producers.reserve(counts.producers);
            for (std::uint32_t i = 0; i < counts.producers; ++i)
                producers.push_back(
                    coop.make_agent<producer_t>(to_manager, i, counts.items, prod_iters)
                        ->so_direct_mbox());
            std::vector<so_5::mbox_t> consumers;
            consumers.reserve(counts.consumers);
            for (std::uint32_t j = 0; j < counts.consumers; ++j)
                consumers.push_back(
                    coop.make_agent<consumer_t>(to_manager, j, cons_iters)->so_direct_mbox());
            manager->wire(std::move(producers), std::move(consumers));
        });
    });
    qvo::Answer answer{sink.checksum, sink.messages};
    answer.observed[kObservedProducerWaits] = sink.producer_waits;
    answer.observed[kObservedConsumerWaits] = sink.consumer_waits;
    answer.observed[kObservedBufferPeak]    = sink.buffer_peak;
    return answer;
}

}  // namespace savina_bndbuffer_sobjectizer

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
    spec.idiom_source      = "the bank-transaction adapter: agents on direct mboxes, "
                             "one coop on qvoso::make_pool_binder, readiness from so_evt_start";
    spec.idiom_note        = "Savina's manager, producers and consumers one for one as agents on "
                             "direct mboxes; the bounded buffer is the manager's FIFO and Savina's "
                             "park/un-park protocol; a producer goes silent after its exit "
                             "message (an agent ends only with its coop); thread_pool(cores) with "
                             "fifo_t::individual for cores>=2";
    spec.caveats           = qvoso::pool_caveats();
    spec.caveats.emplace_back(
        "SObjectizer ends an agent only by deregistering its whole coop, so the 40 producers, "
        "which the reference ends inside the window, stay registered and idle until the manager "
        "stops the environment after it; qb and CAF end each producer as the reference does "
        "(benchmarks/savina/bndbuffer.md)");

    return qvo::run(argc, argv, std::move(spec), savina_bndbuffer_sobjectizer::body);
}
