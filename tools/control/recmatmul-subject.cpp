// The negative control's recmatmul subject: savina/recmatmul's verification, watched rejecting each
// fault class the shape can have and accepting an interleaving no framework controls
// (Huly QB-246).
//
// Savina's master and `workers` workers simulated on one thread, each with a mailbox per sender --
// a worker's holds the master's tasks, the master's one FIFO per worker its created tasks and
// dones: the per-sender order every framework guarantees and Savina's done-count termination rests
// on -- splitting through the spec's own split(), multiplying through multiply(), routing through
// route(), and reducing through checksum() and delivered(), like every adapter. The next delivery
// is the first non-empty mailbox in a fixed scan. `QVO_CONTROL_PLANT` makes it wrong in one way at
// a time:
//
//   none           correct -- the POSITIVE control: must verify.
//   shuffled       correct, every next delivery drawn from a random non-empty mailbox (a fixed
//                  seed), each sender's order kept: must verify -- the two products of a quarter
//                  reach its worker in another order, and every one of them is an exact integer,
//                  so no order moves a bit of C.
//   duplicate      the first leaf delivered twice: its product added twice.
//   lost-child     the root's third created task lost on its way to the master: the run still
//                  ENDS -- every task the master sent has its done -- with a quarter of the
//                  product missing.
//   misroute       the first leaf routed to the next worker: C is right (the product lands where
//                  it should, the worker adds where the task says) and only the per-worker tallies
//                  see it. The subject says so on stderr ("every entry as the closed form says:
//                  yes").
//   wrong-inner    the first leaf multiplies the OTHER half of the inner index (scA and srB moved
//                  by its dimension): the quarter gets its sibling's product twice. With Savina's
//                  own data (A[i][k] = i, B[k][j] = j) every half gives the same product and
//                  Savina's valid() accepts the run; the subject replays every product on that
//                  data and says so on stderr ("Savina's data would accept this run: yes") -- the
//                  reason the spec's data deviates.
//   untouched      the first leaf answered with a done and no product.
//
// Each plant must be REJECTED by the checksum. Run with --param length=100 (not a power of two),
// the spec must refuse the run before anything is built (exit 2).
//
// Like the subjects beside it, it is NOT built by qvo_add_benchmark and its name does not start
// with "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/recmatmul.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace qvospec::savina::recmatmul;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

// What a worker sends the master: a created task, or a done.
struct Up {
    bool done{false};
    Task task;
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config      c    = config(p);
    const std::string mode = plant();

    Matrices m(c.length);
    // The same products on Savina's own data, A[i][k] = i and B[k][j] = j, for the wrong-inner
    // report: what Savina's valid() would have said about this run.
    Matrices savina(c.length);
    for (std::size_t i = 0; i < savina.n; ++i)
        for (std::size_t j = 0; j < savina.n; ++j) {
            savina.a[i * savina.stride + j] = static_cast<double>(i);
            savina.b[i * savina.stride + j] = static_cast<double>(j);
        }

    MasterCounts                 counts;
    std::vector<Tally>           tallies(c.workers);
    std::vector<std::deque<Task>> inbox(c.workers);   // each worker's, from the master
    std::vector<std::deque<Up>>   master(c.workers);  // the master's, one FIFO per worker
    std::mt19937_64              rng(0x7265636d);
    bool                         planted = false, root_split = false, done = false;
    unsigned                     root_children = 0;

    auto send = [&](const Task &task) {
        ++counts.sent;
        std::uint32_t w = route(task, c);
        if (mode == "misroute" && !planted && is_leaf(task, c)) {
            planted = true;
            w       = (w + 1) % c.workers;
        }
        inbox[w].push_back(task);
    };

    // A worker's turn: Savina's Worker.process(WorkMessage).
    auto work = [&](std::uint32_t w) {
        const Task msg = inbox[w].front();
        inbox[w].pop_front();
        Tally &t = tallies[w];
        ++t.works;
        if (is_leaf(msg, c)) {
            ++t.leaves;
            Task task = msg;
            if (!planted && mode == "wrong-inner" && task.scA % (2 * task.dim) == 0) {
                planted = true;
                task.scA += task.dim;
                task.srB += task.dim;
            }
            bool skip = false;
            if (!planted && mode == "untouched") planted = skip = true;
            if (!planted && mode == "duplicate") {
                planted = true;
                inbox[w].push_front(msg);  // the same delivery, again on this worker's next turn
            }
            if (!skip) {
                multiply(m, task);
                multiply(savina, task);
            }
        } else {
            const bool root_task = !root_split;
            root_split           = true;
            ++t.splits;
            split(msg, [&](const Task &child) {
                if (root_task && mode == "lost-child" && ++root_children == 3) return;
                master[w].push_back({false, child});
            });
        }
        master[w].push_back({true, {}});
    };

    // The master's turn on worker w's FIFO: Savina's Master.process.
    auto relay = [&](std::uint32_t w) {
        const Up up = master[w].front();
        master[w].pop_front();
        if (!up.done) {
            ++counts.created;
            send(up.task);
            return;
        }
        if (++counts.dones == counts.sent) {  // the run ends at the last done
            watch.stop();
            done = true;
        }
    };

    watch.start();
    send(root(c));
    std::vector<std::uint32_t> ready;  // non-empty mailboxes: w < workers a worker's, else the master's
    while (!done) {
        ready.clear();
        for (std::uint32_t w = 0; w < c.workers; ++w) {
            if (!inbox[w].empty()) ready.push_back(w);
            if (!master[w].empty()) ready.push_back(c.workers + w);
        }
        if (ready.empty()) break;  // nothing left to deliver: a lost done would block a framework
        const std::uint32_t pick =
            mode == "shuffled" ? ready[static_cast<std::size_t>(rng() % ready.size())] : ready.front();
        if (pick < c.workers) work(pick);
        else relay(pick - c.workers);
    }

    // What C and Savina's own check would say, for the battery's two "only one check sees it"
    // controls.
    const std::uint64_t n  = c.length;
    const std::uint64_t s1 = n * (n - 1) / 2;
    const std::uint64_t s2 = (n - 1) * n * (2 * n - 1) / 6;
    bool                c_right = true, savina_accepts = true;
    for (std::uint64_t i = 0; i < n; ++i)
        for (std::uint64_t j = 0; j < n; ++j) {
            const std::uint64_t a = i + 1, b = j + 1;
            const double        want = static_cast<double>(n * a * b + (2 * a + b) * s1 + 2 * s2);
            c_right = c_right && m.c[i * m.stride + j] == want;
            savina_accepts = savina_accepts
                             && savina.c[i * savina.stride + j] == static_cast<double>(n * i * j);
        }
    std::fprintf(stderr, "recmatmul subject: every entry as the closed form says: %s\n",
                 c_right ? "yes" : "no");
    std::fprintf(stderr, "recmatmul subject: Savina's data would accept this run: %s\n",
                 savina_accepts ? "yes" : "no");
    return qvo::Answer{checksum(m, tallies, counts), delivered(tallies, counts)};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/recmatmul";
    spec.framework         = "control";
    spec.framework_version = plant();
    spec.params            = qvospec::savina::recmatmul::params();
    spec.expected          = qvospec::savina::recmatmul::expected;
    spec.expected_messages = qvospec::savina::recmatmul::expected_messages;
    spec.work_unit         = qvospec::savina::recmatmul::kWorkUnit;
    spec.work_units        = qvospec::savina::recmatmul::work_units;
    spec.idiom_source      = "none -- this is a negative-control subject, not a framework";
    spec.idiom_note        = "Savina's master and its workers simulated on one thread; plants a "
                             "defect selected by QVO_CONTROL_PLANT";
    spec.caveats           = {"NOT A FRAMEWORK AND NOT A RESULT. Exists only so the verifier can "
                              "be watched rejecting something."};

    return qvo::run(argc, argv, std::move(spec), body);
}
