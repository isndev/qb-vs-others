// @benchmark     savina/concsll
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the list is
//                actor 0 on worker 0, alone; the master and every worker are owned by worker 1
//                (worker w by 1 + w % (cores - 1) above two cores) -- the placement qb's cell
//                fixes.
// @idiom-note    A request is one ring push to worker 0, its answer one push back, and the list is
//                the same SortedList every framework's list actor holds -- so what the floor engineers
//                out is only the messaging: no mailbox per actor (two threads share one SPSC ring per
//                direction), no event header, no dispatch. The walk, which dominates the window, is
//                the same code; the floor bounds what the frameworks add around it. There is no
//                request/response primitive to compare, so form=1 is not applicable: this form=0
//                cell is the floor for both forms.

#include <qvospec/savina/concsll.h>

#include "../baseline_support.h"

#include <vector>

namespace savina_concsll_baseline {

using namespace qvospec::savina::concsll;

// kWrite / kContains / kSize (to the list): a = request id, b = the value (as a uint32).
// kAnswer (to a worker):                     a = request id, b = kind << 32 | the answer (as a uint32).
// kDoWork (to a worker): none.               kEnd (to the master): a = the worker's sum, b = its received.
enum Tag : std::uint32_t { kAnswer = 11, kDoWork = 12, kEnd = 13 };

// One cache line per worker slot: slot w is written by the thread that owns worker w, and two slots
// owned by different threads must not share a line.
struct alignas(qvobase::kCacheLine) WorkerSlot {
    Script        script;
    Request       asked{};
    std::uint64_t seq{0};
    std::uint64_t acc{0};
    std::uint64_t received{0};
    explicit WorkerSlot(Script s) noexcept : script(s) {}
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config   c     = Config::of(p);
    const auto     cores = static_cast<unsigned>(p.get("cores"));
    const bool     spin  = p.get("wait") != 0;
    const unsigned W     = cores < 1 ? 1u : cores;
    if (asks(p))
        qvo::not_applicable("the floor has no request/response primitive to measure: a request is a "
                            "ring push and its answer a push back in both forms, so the form=0 cell "
                            "is the floor for form=1 too");

    // Which contains answers are fixed: framework-free, before the window (setup).
    const std::vector<std::uint8_t> written = written_values(c);

    // Actor ids. The Mesh runs actor d on worker d % W, so an id is chosen per actor whose remainder
    // is the worker the placement wants and whose quotient tells the actors apart: the list is 0
    // (worker 0), worker w is W * (w + 1) + worker_core(w, W), the master W * (workers + 1) +
    // master_core(W).
    const std::uint32_t list     = 0;
    auto                actor_of = [W](std::uint32_t w) { return W * (w + 1) + worker_core(w, W); };
    auto                index_of = [W](std::uint32_t actor) { return actor / W - 1; };
    const std::uint32_t master   = W * (c.workers + 1) + master_core(W);

    SortedList              sorted;
    std::uint64_t           list_received = 0;
    std::vector<WorkerSlot> slots;
    slots.reserve(c.workers);
    for (std::uint32_t w = 0; w < c.workers; ++w) slots.emplace_back(Script(w, c));
    std::uint32_t ended           = 0;
    std::uint64_t master_received = 0;
    std::uint64_t result          = 0;
    std::uint64_t delivered       = 0;

    // Worker w's next request, from the thread that owns it.
    auto issue = [&](auto &m, unsigned worker, std::uint32_t w) {
        WorkerSlot &s = slots[w];
        s.asked       = s.script.next();
        m.send(worker, qvobase::Msg{list, s.asked.kind, request_id(w, s.seq),
                                    static_cast<std::uint32_t>(s.asked.value)});
    };
    auto finish = [&](auto &m, unsigned worker, std::uint32_t w) {
        m.send(worker, qvobase::Msg{master, kEnd, slots[w].acc, slots[w].received});
    };

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        switch (msg.tag) {
        case kWrite:
        case kContains:
        case kSize: {  // the list
            ++list_received;
            const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(msg.b));
            std::int32_t answer;
            if (msg.tag == kWrite) {
                sorted.add(value);
                answer = value;
            } else if (msg.tag == kContains) {
                answer = sorted.contains(value) ? 1 : 0;
            } else {
                answer = sorted.size();
            }
            const auto w = static_cast<std::uint32_t>(msg.a >> 32);
            m.send(worker, qvobase::Msg{actor_of(w), kAnswer, msg.a,
                                        (std::uint64_t{msg.tag} << 32) | static_cast<std::uint32_t>(answer)});
            break;
        }
        case kDoWork: {
            const std::uint32_t w = index_of(msg.dst);
            ++slots[w].received;
            if (c.messages == 0)
                finish(m, worker, w);
            else
                issue(m, worker, w);
            break;
        }
        case kAnswer: {
            const std::uint32_t w = index_of(msg.dst);
            WorkerSlot         &s = slots[w];
            ++s.received;
            if (s.seq >= c.messages) fail("a worker answered after its last request");
            const auto kind  = static_cast<std::uint32_t>(msg.b >> 32);
            const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(msg.b));
            s.acc += reply_term(request_id(w, s.seq), s.asked.kind, msg.a, kind,
                                asserted_result(s.asked, value, written));
            if (++s.seq == c.messages)
                finish(m, worker, w);
            else
                issue(m, worker, w);
            break;
        }
        case kEnd:  // the master
            ++master_received;
            result += msg.a;
            delivered += msg.b;
            if (++ended == c.workers) {
                watch.stop();
                m.stop();
            }
            break;
        }
    });

    mesh.start();
    watch.start();
    for (std::uint32_t w = 0; w < c.workers; ++w) mesh.send(0, qvobase::Msg{actor_of(w), kDoWork, 0, 0});
    if (c.workers == 0) {  // nothing will ever call stop() from a handler
        watch.stop();
        mesh.stop();
    }
    mesh.run();

    // After the window, every thread joined: the list's contents and counts.
    result += sorted.fold();
    delivered += list_received + master_received;
    qvo::Answer answer{result, delivered};
    answer.observed = observations(sorted.stats());
    return answer;
}

}  // namespace savina_concsll_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::concsll::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::concsll::expected;
    spec.expected_messages = qvospec::savina::concsll::expected_messages;
    spec.observed_at_least = qvospec::savina::concsll::observed_at_least();
    spec.work_unit         = qvospec::savina::concsll::kWorkUnit;
    spec.work_units        = qvospec::savina::concsll::work_units;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) pair; the list "
                             "is owned by thread 0 alone, the master and the workers by thread 1; a request "
                             "is a push and its answer a push back; the list is the shared SortedList; not "
                             "an actor framework and not ranked as one";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. The list and the workers are slots with no mailbox; a request is one "
        "push into a ring two threads share and its answer one push back -- the walk is the same "
        "SortedList every framework's list actor holds, so the floor bounds the messaging around it",
        "the list lives on thread 0 alone and the master and the workers on thread 1 -- the same "
        "placement qb's cell fixes -- so with cores=2 every request and every answer crosses a core",
        "cores=1 is one thread with the list, the master and the workers in its own ring -- the floor "
        "for single-threaded dispatch, still a real queue",
        "wait=1 busy-polls the rings; wait=0 parks an idle thread on a condition variable -- at cores=2 "
        "the workers' thread has nothing to do while the list walks, so it may park between answers "
        "and be woken by the next one",
        qvospec::savina::concsll::kSharedListCaveat};

    return qvo::run(argc, argv, std::move(spec), savina_concsll_baseline::body);
}
