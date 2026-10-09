// @benchmark     savina/concdict
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the master
//                and the dictionary are the two roles of actor 0, owned by worker 0 and told apart
//                by the message tag; worker w is actor 1 + w, owned by worker (1 + w) % cores --
//                the placement qb's cell fixes.
// @idiom-note    A request is one ring push to the dictionary's thread carrying the worker's
//                index, the answer one push back; a worker is a slot of a vector (its request
//                index, its fold, its count) with no mailbox. There is no request/response
//                primitive to compare, so form=1 is not applicable: this form=0 cell is the floor
//                for both forms. What the floor lacks, and every framework pays for, is a mailbox
//                per actor and a twenty-writer fan-in into the dictionary's: here each pair of
//                threads shares one SPSC ring per direction.

#include <qvospec/savina/concdict.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_concdict_baseline {

using namespace qvospec::savina::concdict;

// kStart: none.   kRequest: a = worker << 32 | key, b = write << 32 | value.   kAnswer: a = value.
// kDone: a = received << 32 | worker, b = fold.   kExit: none.   kReport: a = received, b = digest.
enum Tag : std::uint32_t {
    kStart   = 1,
    kRequest = 2,
    kAnswer  = 3,
    kDone    = 4,
    kExit    = 5,
    kReport  = 6
};

struct WorkerSlot {
    std::uint64_t next{0};  // the index of the request in flight
    std::uint64_t fold{0};
    std::uint64_t received{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Shape    shape = qvospec::savina::concdict::shape(p);
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;
    if (asks(p))
        qvo::not_applicable("the floor has no request/response primitive to measure: a request is a "
                            "ring push and its answer a push back in both forms, so the form=0 cell "
                            "is the floor for form=1 too");

    const std::uint32_t center    = 0;  // the master and the dictionary, by tag
    auto                actor_of  = [](std::uint32_t w) { return 1 + w; };
    auto                worker_of = [](std::uint32_t actor) { return actor - 1; };

    Store                   store(shape.keys);  // before the threads, by this thread -- as in every adapter
    std::vector<WorkerSlot> workers(shape.workers);
    std::uint64_t           dictionary_received = 0;
    std::uint64_t           master_received     = 0;
    std::uint32_t           done                = 0;
    std::uint64_t           result              = 0;
    std::uint64_t           delivered           = 0;

    auto request = [&](auto &m, unsigned thread, std::uint32_t w, std::uint64_t j) {
        const Operation o = operation(shape, w, j);
        m.send(thread, qvobase::Msg{center, kRequest, (std::uint64_t{w} << 32) | o.key,
                                    (std::uint64_t{o.write} << 32) | o.value});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned thread, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kStart: {
            const std::uint32_t w = worker_of(msg.dst);
            ++workers[w].received;
            request(m, thread, w, 0);
            break;
        }
        case kRequest: {  // the dictionary
            ++dictionary_received;
            const auto w     = static_cast<std::uint32_t>(msg.a >> 32);
            const auto key   = static_cast<std::uint32_t>(msg.a & 0xffffffffu);
            const auto value = static_cast<std::uint32_t>(msg.b & 0xffffffffu);
            const auto answer = (msg.b >> 32) != 0 ? store.write(key, value) : store.read(key);
            m.send(thread, qvobase::Msg{actor_of(w), kAnswer, answer, 0});
            break;
        }
        case kAnswer: {
            const std::uint32_t w = worker_of(msg.dst);
            WorkerSlot         &s = workers[w];
            ++s.received;
            if (s.next >= shape.messages) fail("a worker answered after its last request");
            s.fold += reply_key(w, s.next, static_cast<std::uint32_t>(msg.a));
            if (++s.next == shape.messages) {
                m.send(thread, qvobase::Msg{center, kDone, (s.received << 32) | w, s.fold});
                break;
            }
            request(m, thread, w, s.next);
            break;
        }
        case kDone:  // the master
            ++master_received;
            result += done_key(static_cast<std::uint32_t>(msg.a & 0xffffffffu), msg.b);
            delivered += msg.a >> 32;
            if (++done == shape.workers) {
                watch.stop();  // every request answered and every worker done
                m.send(thread, qvobase::Msg{center, kExit, 0, 0});
            }
            break;
        case kExit:  // the dictionary
            ++dictionary_received;
            m.send(thread, qvobase::Msg{center, kReport, dictionary_received, store.digest()});
            break;
        case kReport:  // the master
            ++master_received;
            result += digest_key(msg.b);
            delivered += msg.a + master_received;
            m.stop();
            break;
        }
    });

    mesh.start();
    watch.start();
    for (std::uint32_t w = 0; w < shape.workers; ++w)
        mesh.send(0, qvobase::Msg{actor_of(w), kStart, 0, 0});
    mesh.run();
    return qvo::Answer{result, delivered};
}

}  // namespace savina_concdict_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::concdict::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::concdict::expected;
    spec.expected_messages = qvospec::savina::concdict::expected_messages;
    spec.work_unit         = qvospec::savina::concdict::kWorkUnit;
    spec.work_units        = qvospec::savina::concdict::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (thread, thread) pair; "
                             "master and dictionary on thread 0, worker w on thread (1 + w) % cores; "
                             "a request is a push carrying the worker's index, the answer a push "
                             "back; not an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. Workers are slots of a vector with no mailbox, and the "
        "dictionary's twenty-writer fan-in is a scan of one SPSC ring per thread, not a shared "
        "mailbox: the per-actor queue every framework keeps is engineered out",
        "the master and the dictionary live on thread 0 and worker w on thread (1 + w) % cores -- "
        "the same placement qb's cell fixes -- so with cores=2 half the round trips cross a core",
        "no request/response primitive: the form=1 documents are not applicable here, by the "
        "harness's third verdict, and this form=0 cell is the floor for both forms",
        "the dictionary is the spec's Store (one std::unordered_map of `keys` entries, the same "
        "object code in every adapter), built before the threads by the thread that runs the "
        "repetition; its lookups are part of the floor's figure as of every framework's",
        "cores=1 is one thread with every role in its own ring -- the floor for single-threaded "
        "dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle thread on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_concdict_baseline::body);
}
