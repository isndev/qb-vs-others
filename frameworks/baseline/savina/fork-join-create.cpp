// @benchmark     savina/fork-join-create
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h, with the
//                fib floor's notion of an "actor" born and dead inside the window: a heap node in
//                a per-worker slot table, allocated by its creator's worker and freed after it has
//                answered.
// @idiom-note    The floor for forking an actor is what nothing can avoid: one `new`, one slot in
//                a table, one `delete`. Ids are `slot * W + w`, so a forked actor stays on its
//                creator's worker (owner = id % W) and no table is ever touched by two threads --
//                the same static placement qb's addRefActor has, and no balancing, which is what
//                the frameworks with a pool pay for. What a framework adds on top is a mailbox, a
//                registry the id resolves through, and the lifecycle bookkeeping.

#include <qvospec/savina/fork-join-create.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_fork_join_create_baseline {

using namespace qvospec::savina::fork_join_create;

// kFork: no payload.  kJob: a = the job's index.  kDone: a = value, b = messages.
// kSummary: a = chk, b = messages.
enum Tag : std::uint32_t { kFork = 1, kJob = 2, kDone = 3, kSummary = 4 };

// A forked actor holds its creator and its own index; a creator (and the driver) holds the fold.
struct Node {
    std::uint32_t creator{0};
    std::uint64_t self{0};
    std::uint64_t first{0};
    std::uint64_t share{0};
    std::uint64_t acc{0};
    std::uint64_t messages{0};
    std::uint64_t done{0};
};

// One worker's actors. Touched only by the thread owning the worker (and by the main thread
// before start, for the driver and the creators).
struct Table {
    std::vector<Node *>        slots;
    std::vector<std::uint32_t> free_slots;

    std::uint32_t alloc(unsigned w, unsigned W, const Node &init) {
        std::uint32_t slot;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(slots.size());
            slots.push_back(nullptr);
        }
        slots[slot] = new Node{init};
        return slot * W + w;
    }

    void release(std::uint32_t slot) {
        delete slots[slot];
        slots[slot] = nullptr;
        free_slots.push_back(slot);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     actors = static_cast<std::uint64_t>(p.get("actors"));
    const auto     work   = static_cast<int>(p.get("work"));
    const auto     ncreat = creators(p);
    const bool     spin   = p.get("wait") != 0;
    const unsigned W      = static_cast<unsigned>(ncreat);

    // The driver is id 0 on worker 0 (slot 0 of table 0); creator s lives on worker s. None of
    // them is freed before the end of the run.
    std::vector<Table>         tables(W);
    const std::uint32_t        driver = tables[0].alloc(0, W, Node{});
    std::vector<std::uint32_t> creator_ids(ncreat);
    for (std::uint64_t s = 0; s < ncreat; ++s) {
        Node init{};
        init.first = s;
        init.share = share(actors, ncreat, s);
        creator_ids[s] =
            tables[static_cast<unsigned>(s)].alloc(static_cast<unsigned>(s), W, init);
    }

    std::uint64_t result    = 0;
    std::uint64_t delivered = 0;
    std::uint64_t summaries = 0;

    auto summarise = [&](auto &m, unsigned worker, Node &c) {
        m.send(worker, qvobase::Msg{driver, kSummary, c.acc, c.messages});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kFork: {
            // `c` is a heap node: alloc() may grow the slot vector, never move the node.
            Node &c = *tables[worker].slots[msg.dst / W];
            ++c.messages;
            for (std::uint64_t i = c.first; i < actors; i += ncreat) {
                Node init{};
                init.creator = msg.dst;
                init.self    = i;
                const std::uint32_t a = tables[worker].alloc(worker, W, init);
                // A send to a full ring owned by this worker drains inline, which can run the
                // forked actor's job and deliver its done to `c` before send() returns; the fold
                // below is written so that is harmless.
                m.send(worker, qvobase::Msg{a, kJob, i, 0});
            }
            if (c.share == 0) summarise(m, worker, c);  // more creators than actors
            break;
        }
        case kJob: {
            const std::uint32_t slot = msg.dst / W;
            const Node          f    = *tables[worker].slots[slot];
            tables[worker].release(slot);
            m.send(worker, qvobase::Msg{f.creator, kDone, job_value(f.self, msg.a, work), 1});
            break;
        }
        case kDone: {
            Node &c = *tables[worker].slots[msg.dst / W];
            c.acc += msg.a;
            c.messages += 1 + msg.b;
            if (++c.done == c.share) summarise(m, worker, c);
            break;
        }
        case kSummary: {
            result += msg.a;
            delivered += 1 + msg.b;
            if (++summaries == ncreat) {
                watch.stop();
                m.stop();
            }
            break;
        }
        }
    });
    mesh.start();

    watch.start();
    for (std::uint64_t s = 0; s < ncreat; ++s) mesh.send(0, qvobase::Msg{creator_ids[s], kFork, 0, 0});
    mesh.run();

    for (std::uint64_t s = 0; s < ncreat; ++s)
        tables[static_cast<unsigned>(s)].release(creator_ids[s] / W);
    tables[0].release(driver / W);
    return qvo::Answer{result, delivered};
}

}  // namespace savina_fork_join_create_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::fork_join_create::params();
    spec.expected          = qvospec::savina::fork_join_create::expected;
    spec.expected_messages = qvospec::savina::fork_join_create::expected_messages;
    spec.work_unit         = qvospec::savina::fork_join_create::kWorkUnit;
    spec.work_units        = qvospec::savina::fork_join_create::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; creator s on worker s forks its share as heap nodes in its "
                             "worker's slot table, each freed when it answers; not an actor "
                             "framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A forked actor is one `new`, one slot and one `delete`, with no "
        "mailbox, no registry and no lifecycle beyond the slot: the floor for what creating and "
        "destroying an actor can cost",
        "a forked actor is allocated on its creator's worker (id = slot * cores + worker), the "
        "same static placement qb's addRefActor has: with cores=2 each worker forks and runs its "
        "own half, so this floor is a bound for the placing frameworks and NOT for the pools",
        "cores=1 is one thread with every actor in its own ring -- the floor for single-threaded "
        "dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_fork_join_create_baseline::body);
}
