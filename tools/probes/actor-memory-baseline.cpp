// Raw-node memory floor, not an actor framework or a contestant.
#include <qvo/actor-memory.h>
#include <exception>
#include "../../frameworks/baseline/baseline_support.h"

namespace actor_memory_floor {
struct Node {
    qvo::probe::PopulationLedger &ledger;
    std::size_t index;
    Node(qvo::probe::PopulationLedger &value, std::size_t id) : ledger(value), index(id) { ledger.born(index); }
    ~Node() { ledger.destroyed(index); }
};
} // namespace actor_memory_floor

int main(int argc, char **argv) {
    std::vector<std::unique_ptr<actor_memory_floor::Node>> nodes;
    qvo::probe::MemoryAdapter adapter;
    adapter.framework = "baseline";
    adapter.version = QVO_FRAMEWORK_VERSION;
    adapter.caveat = "Raw heap nodes plus the existing Mesh fixed-worker runtime; no framework services. "
                     "Node pointer table is touched before baseline. Each node receives one readiness message and stays allocated. "
                     "Nodes are allocated on the controller thread; no per-actor mailbox/subscription allocation.";
    adapter.floor = true;
    adapter.object_size = sizeof(actor_memory_floor::Node);
    adapter.prepare = [&](const auto &config) { nodes.resize(config.actors); };
    adapter.run = [&](const auto &config, auto &ledger, const auto &capture) {
        struct ResetNodes {
            decltype(nodes) &value;
            ~ResetNodes() { for (auto &node : value) node.reset(); }
        } reset{nodes};
        for (std::size_t i = 0; i < qvo::probe::memory_spawn_count<(QVO_MEMORY_CONTROL_DROP_LAST != 0)>(config.actors); ++i)
            nodes[i] = std::make_unique<actor_memory_floor::Node>(ledger, i);
        qvobase::Mesh mesh(config.cores, false, [&](auto &, unsigned, const qvobase::Msg &msg) {
            if (msg.tag == 1) ledger.infrastructure_ready_signal(msg.dst);
            else if (msg.tag == 2 && msg.a < nodes.size() && nodes[msg.a]) ledger.ready(msg.a);
        });
        std::exception_ptr worker_error;
        std::thread worker([&] {
            try {
                try { mesh.start(); }
                catch (...) {
                    // start() may have created only some workers. stop()+run()
                    // joins that partial set; never rerun after a successful run().
                    const auto startup_error = std::current_exception();
                    mesh.stop();
                    mesh.run();
                    std::rethrow_exception(startup_error);
                }
                for (unsigned c = 0; c < config.cores; ++c) mesh.send(0, {c, 1, 0, 0});
                for (std::size_t i = 0; i < qvo::probe::memory_spawn_count<(QVO_MEMORY_CONTROL_DROP_LAST != 0)>(config.actors); ++i)
                    mesh.send(0, {static_cast<std::uint32_t>(i), 2, i, 0});
                mesh.run();
            } catch (...) { worker_error = std::current_exception(); }
        });
        auto stop = [&] {
            mesh.stop();
            worker.join();
        };
        try { capture(); }
        catch (...) {
            stop();
            if (worker_error) std::rethrow_exception(worker_error);
            throw;
        }
        stop();
        if (worker_error) std::rethrow_exception(worker_error);
    };
    return qvo::probe::actor_memory_main(argc, argv, adapter);
}
