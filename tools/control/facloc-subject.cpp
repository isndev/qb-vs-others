// The negative control's facloc subject: savina/facloc's verification, watched rejecting each fault
// its answer and its protocol checks exist to see.
//
// savina/facloc's answer is the QUADTREE: which quadrants opened a facility, which customers each
// of the others holds, the facility notifications each quadrant received and every delivery,
// summed up the tree by Savina's own ConfirmExit convergecast; the spec's reference fixes every one
// of them for every interleaving. This subject runs the protocol on one thread -- a FIFO of
// messages, the producer and every quadrant a record holding the spec's kernel -- through the
// spec's own Quadrant, Tally and checksum(), exactly as the four adapters do, the adapters'
// protocol checks included. `QVO_CONTROL_PLANT` makes it wrong in one way at a time:
//
//   none                 correct -- the POSITIVE control: must verify.
//   misroute-customer    one customer below the root goes to a sibling of the child whose region
//                        holds it.
//   duplicate-customer   one customer below the root is delivered twice.
//   lost-customer        one customer below the root is lost -- the producer's flow control sees
//                        only the root, so nothing blocks.
//   no-split             one quadrant never splits, whatever its cost says.
//   lost-facility        one Facility notification is lost on its way up.
//   duplicate-exit       one RequestExit is delivered twice.
//   untouched            one quadrant passes a customer on without its handler counting it.
//
// Each plant must be REJECTED by the harness: by the checksum, by the message count, or by the
// protocol check that sees it first (savina/facloc: protocol violated -- ...). Run with
// --param points=0, the spec must refuse the run before anything is built (exit 2).
//
// Like the other subjects, it is NOT built by qvo_add_benchmark and its name does not start with
// "qvo-": tools/run.py cannot discover it, and no planted result can reach a table.

#include <qvospec/savina/facloc.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace qvospec::savina::facloc;

std::string plant() {
    if (const char *v = std::getenv("QVO_CONTROL_PLANT")) return v;
    return "none";
}

enum Kind { kCustomer, kNext, kFacility, kExit, kConfirm };

constexpr std::uint32_t kProducer = 0xffffffffu;

struct Message {
    Kind          kind{kCustomer};
    std::uint32_t to{0};  // a quadrant's index (the root is 0), or kProducer
    Customer      c{};
    std::uint64_t fid{0};
    Tally         tally{};
};

struct Quad {
    std::uint32_t                parent{0};
    std::optional<Quadrant>      q;
    std::array<std::uint32_t, 4> children{};
    std::uint32_t                confirms{0};
    bool                         exiting{false};
    Tally                        tally{};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config                cfg   = config(p);
    const std::vector<Customer> all   = customers(cfg);
    const std::string           which = plant();

    std::vector<Quad> quads(1);
    quads[0].q.emplace(root_seed(cfg), cfg.threshold);
    std::deque<Message> queue;
    std::size_t         produced          = 0;
    std::uint64_t       producer_messages = 0;
    bool                done              = false;
    bool                planted           = false;  // each plant fires once
    std::uint32_t       no_split          = kProducer;  // the quadrant the no-split plant picked
    // The plants below the root wait for the tree to be a few levels deep.
    auto ready = [&](std::uint32_t n) { return !planted && n != 0 && quads[n].q->depth() >= 2; };

    auto confirm = [&](std::uint32_t n) {
        Quad &x = quads[n];
        x.tally.own(*x.q, n == 0);
        if (n == 0) {
            watch.stop();
            done = true;
            return;
        }
        Message m;
        m.kind  = kConfirm;
        m.to    = x.parent;
        m.tally = x.tally;
        queue.push_back(m);
    };
    auto customer_to = [&](std::uint32_t to, const Customer &c) {
        Message m;
        m.kind = kCustomer;
        m.to   = to;
        m.c    = c;
        queue.push_back(m);
    };

    watch.start();
    customer_to(0, all[produced++]);
    while (!queue.empty() && !done) {
        const Message m = queue.front();
        queue.pop_front();
        switch (m.kind) {
        case kCustomer: {
            const std::uint32_t n = m.to;
            if (which == "untouched" && ready(n) && quads[n].q->open()) {
                planted = true;
                customer_to(quads[n].children[quads[n].q->route(m.c)], m.c);
                break;
            }
            ++quads[n].tally.messages;
            if (quads[n].exiting) fail("a customer after RequestExit");
            if (quads[n].q->open()) {
                unsigned k = quads[n].q->route(m.c);
                if (ready(n) && which == "misroute-customer") {
                    planted = true;
                    k       = (k + 1) % 4;
                }
                if (ready(n) && which == "lost-customer") planted = true;
                else customer_to(quads[n].children[k], m.c);
                if (ready(n) && which == "duplicate-customer") {
                    planted = true;
                    customer_to(quads[n].children[k], m.c);
                }
            } else if (quads[n].q->add(m.c)) {
                if (no_split == n || (ready(n) && which == "no-split")) {
                    planted  = true;  // this quadrant ignores its threshold from now on
                    no_split = n;
                } else {
                    std::array<Seed, 4> seeds = quads[n].q->split();
                    if (n != 0) {
                        Message f;
                        f.kind = kFacility;
                        f.to   = quads[n].parent;
                        f.fid  = quads[n].q->pid();
                        queue.push_back(f);
                    }
                    for (unsigned k = 0; k < 4; ++k) {
                        Quad child;
                        child.parent = n;
                        child.q.emplace(std::move(seeds[k]), cfg.threshold);
                        quads.push_back(std::move(child));
                        quads[n].children[k] = static_cast<std::uint32_t>(quads.size() - 1);
                    }
                }
            }
            if (n == 0) queue.push_back(Message{kNext, kProducer});
            break;
        }
        case kNext: {
            ++producer_messages;
            if (produced < all.size()) customer_to(0, all[produced++]);
            else queue.push_back(Message{kExit, 0});
            break;
        }
        case kFacility: {
            const std::uint32_t n = m.to;
            ++quads[n].tally.messages;
            quads[n].tally.notif_w += m.fid;
            if (n == 0) break;
            Message f = m;
            f.to      = quads[n].parent;
            if (which == "lost-facility" && !planted) planted = true;
            else queue.push_back(f);
            break;
        }
        case kExit: {
            const std::uint32_t n = m.to;
            ++quads[n].tally.messages;
            if (quads[n].exiting) fail("a second RequestExit");
            quads[n].exiting = true;
            if (!quads[n].q->open()) {
                confirm(n);
                break;
            }
            for (std::uint32_t c : quads[n].children) {
                queue.push_back(Message{kExit, c});
                if (which == "duplicate-exit" && !planted) {
                    planted = true;
                    queue.push_back(Message{kExit, c});
                }
            }
            break;
        }
        case kConfirm: {
            const std::uint32_t n = m.to;
            ++quads[n].tally.messages;
            quads[n].tally.add(m.tally);
            if (++quads[n].confirms > 4) fail("a fifth ConfirmExit");
            if (quads[n].confirms == 4) confirm(n);
            break;
        }
        }
    }
    if (!done) watch.stop();  // a plant that starves the protocol: the answer says so
    return qvo::Answer{checksum(quads[0].tally), quads[0].tally.messages + producer_messages};
}

}  // namespace

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = "control/facloc";
    spec.framework         = "control";
    spec.framework_version = "0";
    spec.params            = qvospec::savina::facloc::params();
    spec.expected          = qvospec::savina::facloc::expected;
    spec.expected_messages = qvospec::savina::facloc::expected_messages;
    spec.work_unit         = qvospec::savina::facloc::kWorkUnit;
    spec.work_units        = qvospec::savina::facloc::work_units;
    spec.idiom_source      = "none -- the negative control's facloc subject";
    spec.idiom_note        = "savina/facloc's protocol on one thread through the spec's Quadrant, "
                             "Tally and checksum(), one planted fault at a time (QVO_CONTROL_PLANT)";
    return qvo::run(argc, argv, std::move(spec), body);
}
