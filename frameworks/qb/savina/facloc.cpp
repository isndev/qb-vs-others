// @benchmark     savina/facloc
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("Referenced child (same core)": `addRefActor<T>()` creates a
//                child ON THE SAME VirtualCore, `onInit` run synchronously, the id pushable before
//                the call returns; "Every event payload must be trivially relocatable ... smart
//                pointers are fine") and `kill()`; the uct adapter beside this file for the
//                planter, qb's way to create an actor on another core.
// @idiom-note    A quadrant is an actor holding the spec's kernel; a split creates its four
//                children with `addRefActor`, each constructed from its seed (its region, its
//                corner ancestors' facilities, its share of the customers) and sent nothing else to
//                start. Customers travel down with push<> -- one ordered channel per parent and
//                child, which is what makes every run hold the same quadtree. The producer and the
//                root live on VirtualCore 0; the root's child k on core k % cores: the root creates
//                its own, and asks the planter of core k % cores -- an actor created before the
//                window -- for the others, with the child's seed behind a std::unique_ptr in the
//                Plant event, holding the customers it routes to that child until the planter
//                names it (Planted). Neither is the protocol's, and neither is counted.

#include <qvospec/savina/facloc.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <array>
#include <memory>
#include <vector>

namespace savina_facloc_qb {

using namespace qvospec::savina::facloc;

struct Ready : qb::Event {};
struct CustomerMsg : qb::Event {
    Customer c;
    explicit CustomerMsg(const Customer &cu) noexcept : c(cu) {}
};
struct NextCustomer : qb::Event {};
struct Facility : qb::Event {  // Savina's FacilityMsg from a child: which quadrant opened, how deep
    std::uint64_t fid{0};
    std::uint32_t depth{0};
    Facility(std::uint64_t f, std::uint32_t d) noexcept : fid(f), depth(d) {}
};
struct RequestExit : qb::Event {};
struct ConfirmExit : qb::Event {
    Tally tally;
    explicit ConfirmExit(const Tally &t) noexcept : tally(t) {}
};
struct Plant : qb::Event {  // root -> planter: create my child k from this seed
    std::uint32_t         k{0};
    std::unique_ptr<Seed> seed;
    Plant(std::uint32_t kk, std::unique_ptr<Seed> s) noexcept : k(kk), seed(std::move(s)) {}
};
struct Planted : qb::Event {  // planter -> root: child k is this actor
    std::uint32_t k{0};
    qb::ActorId   child;
    Planted(std::uint32_t kk, qb::ActorId c) noexcept : k(kk), child(c) {}
};

struct Sink {
    Tally         tally;
    std::uint64_t producer_messages{0};
};

struct Field {
    Config                   cfg{};
    std::uint32_t            cores{1};
    qb::ActorId              producer;
    qb::ActorId              root;
    std::vector<qb::ActorId> planters;  // planters[c] for c >= 1
};

// Every quadrant below the root.
class QuadrantActor final : public qb::Actor {
    const Field               &_field;
    const qb::ActorId          _parent;
    Quadrant                   _q;
    std::array<qb::ActorId, 4> _children{};
    std::uint32_t              _confirms{0};
    bool                       _exiting{false};
    Tally                      _tally;

    void split() {
        std::array<Seed, 4> seeds = _q.split();
        push<Facility>(_parent, _q.pid(), _q.depth());
        for (unsigned k = 0; k < 4; ++k) {
            const auto child = addRefActor<QuadrantActor>(std::cref(_field), id(), std::move(seeds[k]));
            if (!child.valid())
                qvo::fail("savina/facloc qb: addRefActor returned an invalid handle -- the "
                          "VirtualCore's 16-bit actor id pool is exhausted");
            _children[k] = child.id();
        }
    }

    void confirm() {
        _tally.own(_q, false);
        push<ConfirmExit>(_parent, _tally);
        kill();
    }

public:
    QuadrantActor(const Field &field, qb::ActorId parent, Seed &&seed)
        : _field(field), _parent(parent), _q(std::move(seed), field.cfg.threshold) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<CustomerMsg>(*this);
        registerEvent<Facility>(*this);
        registerEvent<RequestExit>(*this);
        registerEvent<ConfirmExit>(*this);
        co_return true;
    }

    void on(CustomerMsg const &event) {
        ++_tally.messages;
        if (_exiting) fail("a customer after RequestExit");
        if (_q.open()) push<CustomerMsg>(_children[_q.route(event.c)], event.c);
        else if (_q.add(event.c)) split();
    }

    // Savina's notifyParentOfFacility, on up to the root.
    void on(Facility const &event) {
        ++_tally.messages;
        _tally.notif_w += event.fid;
        push<Facility>(_parent, event.fid, event.depth);
    }

    void on(RequestExit const &) {
        ++_tally.messages;
        if (_exiting) fail("a second RequestExit");
        _exiting = true;
        if (!_q.open()) return confirm();
        for (const qb::ActorId c : _children) push<RequestExit>(c);
    }

    void on(ConfirmExit const &event) {
        ++_tally.messages;
        _tally.add(event.tally);
        if (++_confirms > 4) fail("a fifth ConfirmExit");
        if (_confirms == 4) confirm();
    }
};

// One per core but the root's, created before the window: the only way a qb actor creates an
// actor on another VirtualCore is to ask an actor that lives there.
class PlanterActor final : public qb::Actor {
    const Field &_field;

public:
    explicit PlanterActor(const Field &field) noexcept : _field(field) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Plant>(*this);
        push<Ready>(_field.producer);
        co_return true;
    }

    void on(Plant &event) {
        const auto child =
            addRefActor<QuadrantActor>(std::cref(_field), _field.root, std::move(*event.seed));
        if (!child.valid()) qvo::fail("savina/facloc qb: addRefActor returned an invalid handle at a planter");
        push<Planted>(_field.root, event.k, child.id());
    }
};

// The root quadrant: the kernel, the producer's NextCustomer, and the planters for its children.
class RootActor final : public qb::Actor {
    const Field                         &_field;
    qvo::Watch                          &_watch;
    Sink                                &_sink;
    Quadrant                             _q;
    std::array<qb::ActorId, 4>           _children{};
    std::array<bool, 4>                  _known{};
    std::array<std::vector<Customer>, 4> _held;  // for a child its planter has not named yet
    std::array<bool, 4>                  _exit_held{};
    std::uint32_t                        _confirms{0};
    bool                                 _exiting{false};
    Tally                                _tally;

    void to_child(unsigned k, const Customer &c) {
        if (_known[k]) push<CustomerMsg>(_children[k], c);
        else _held[k].push_back(c);
    }

    void split() {
        std::array<Seed, 4> seeds = _q.split();  // the root has no parent to tell
        for (unsigned k = 0; k < 4; ++k) {
            const std::uint32_t core = k % _field.cores;
            if (core != 0) {
                push<Plant>(_field.planters[core], k, std::make_unique<Seed>(std::move(seeds[k])));
                continue;
            }
            const auto child = addRefActor<QuadrantActor>(std::cref(_field), id(), std::move(seeds[k]));
            if (!child.valid()) qvo::fail("savina/facloc qb: addRefActor returned an invalid handle at the root");
            _children[k] = child.id();
            _known[k]    = true;
        }
    }

    void finish() {
        _tally.own(_q, true);
        _watch.stop();
        _sink.tally = _tally;
        broadcast<qb::KillEvent>();
    }

public:
    RootActor(const Field &field, qvo::Watch &watch, Sink &sink)
        : _field(field), _watch(watch), _sink(sink), _q(root_seed(field.cfg), field.cfg.threshold) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<CustomerMsg>(*this);
        registerEvent<Facility>(*this);
        registerEvent<RequestExit>(*this);
        registerEvent<ConfirmExit>(*this);
        registerEvent<Planted>(*this);
        push<Ready>(_field.producer);
        co_return true;
    }

    void on(CustomerMsg const &event) {
        ++_tally.messages;
        if (_exiting) fail("a customer after RequestExit");
        if (_q.open()) to_child(_q.route(event.c), event.c);
        else if (_q.add(event.c)) split();
        push<NextCustomer>(_field.producer);  // Savina: the root asks for the next one
    }

    void on(Facility const &event) {
        ++_tally.messages;
        _tally.notif_w += event.fid;
    }

    void on(Planted const &event) {
        _children[event.k] = event.child;
        _known[event.k]    = true;
        for (const Customer &c : _held[event.k]) push<CustomerMsg>(event.child, c);
        _held[event.k].clear();
        if (_exit_held[event.k]) push<RequestExit>(event.child);
    }

    void on(RequestExit const &) {
        ++_tally.messages;
        if (_exiting) fail("a second RequestExit");
        _exiting = true;
        if (!_q.open()) return finish();
        for (unsigned k = 0; k < 4; ++k) {
            if (_known[k]) push<RequestExit>(_children[k]);
            else _exit_held[k] = true;
        }
    }

    void on(ConfirmExit const &event) {
        ++_tally.messages;
        _tally.add(event.tally);
        if (++_confirms > 4) fail("a fifth ConfirmExit at the root");
        if (_confirms == 4) finish();
    }
};

class ProducerActor final : public qb::Actor {
    const Field                 &_field;
    const std::vector<Customer> &_customers;
    qvo::Watch                  &_watch;
    Sink                        &_sink;
    std::uint32_t                _ready{0};
    std::size_t                  _produced{0};

    void produce() { push<CustomerMsg>(_field.root, _customers[_produced++]); }

public:
    ProducerActor(const Field &field, const std::vector<Customer> &customers, qvo::Watch &watch,
                  Sink &sink) noexcept
        : _field(field), _customers(customers), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<NextCustomer>(*this);
        co_return true;
    }

    // The window opens once the root and every planter are up: every core running.
    void on(Ready const &) {
        if (++_ready != _field.cores) return;
        _watch.start();
        produce();
    }

    void on(NextCustomer const &) {
        ++_sink.producer_messages;
        if (_produced < _customers.size()) produce();
        else push<RequestExit>(_field.root);
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Field field;
    field.cfg   = config(p);
    field.cores = static_cast<std::uint32_t>(cores < 1 ? 1 : cores);
    const std::vector<Customer> all = customers(field.cfg);  // drawn before the window
    Sink                        sink;
    {
        qb::Main engine;

        const int ncores = static_cast<int>(field.cores);
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.producer = engine.addActor<ProducerActor>(0, std::cref(field), std::cref(all),
                                                        std::ref(watch), std::ref(sink));
        field.root = engine.addActor<RootActor>(0, std::cref(field), std::ref(watch), std::ref(sink));
        field.planters.assign(field.cores, qb::ActorId{});
        for (std::uint32_t c = 1; c < field.cores; ++c)
            field.planters[c] = engine.addActor<PlanterActor>(static_cast<qb::CoreId>(c), std::cref(field));

        engine.start();
        engine.join();
    }
    return qvo::Answer{checksum(sink.tally), sink.tally.messages + sink.producer_messages};
}

}  // namespace savina_facloc_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::facloc::params();
    spec.expected          = qvospec::savina::facloc::expected;
    spec.expected_messages = qvospec::savina::facloc::expected_messages;
    spec.work_unit         = qvospec::savina::facloc::kWorkUnit;
    spec.work_units        = qvospec::savina::facloc::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: addRefActor<T>() (same-core child) + kill(); the "
                             "uct adapter's per-core planter";
    spec.idiom_note        = "a quadrant splits by creating four children with addRefActor from "
                             "their seeds; customers travel down with push<>; producer and root on "
                             "VirtualCore 0, the root's child k on core k % cores through a planter";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb has no cross-core spawn: a quadrant lives on its parent's VirtualCore, so with cores=2 "
        "the root's children 0 and 2 (the left half) and their subtrees run on core 0 with the "
        "producer and the root, children 1 and 3 on core 1, and nothing balances the two; the pools "
        "place every quadrant dynamically -- see benchmarks/savina/facloc.md");
    spec.caveats.emplace_back(
        "the root's children on another core are created by a planter actor of that core (one Plant "
        "event each, the seed behind a std::unique_ptr), and the root holds the customers it routes "
        "to such a child until the planter names it: neither message is counted");

    return qvo::run(argc, argv, std::move(spec), savina_facloc_qb::body);
}
