// @benchmark     savina/uct
// @framework     qb
// @idiom-source  qb/llm/qb.llm.md ("Referenced child (same core)": `addRefActor<T>()` from inside
//                an actor creates a child ON THE SAME VirtualCore, its `onInit` run synchronously,
//                the handle's `id()` valid -- and pushable -- before the call returns) and
//                `kill()`; the fib and fork-join-create adapters beside this file, whose child
//                lifecycle this is; `getSource()` to answer the node that asked.
// @idiom-note    A node told to generate creates its `binomial` children with `addRefActor`, then
//                push<>es each a TryGenerate; a node reports its subtree to its parent with one
//                push<Report> and `kill()`s itself. qb has NO cross-core spawn primitive, so a
//                subtree lives on its top node's core: the root (on VirtualCore 0) creates its own
//                children i with i % cores == 0 and asks a PLANTER -- an actor created before the
//                window, one per other core -- to create the others on its core, which is the qb
//                way to create on another core (one Plant event per core, not counted: it is not
//                the protocol's). The subtrees never move: the imbalance stays where it grew.

#include <qvospec/savina/uct.h>

#include "../qb_support.h"

#include <qb/actor.h>
#include <qb/main.h>

#include <vector>

namespace savina_uct_qb {

using namespace qvospec::savina::uct;

struct Ready : qb::Event {};
struct Plant : qb::Event {};  // root -> planter: create your core's share of my children
struct TryGenerate : qb::Event {};
struct Request : qb::Event {  // Savina's ShouldGenerateChildrenMessage
    std::uint64_t node{0};  // the asking node's identity (a member named `id` would hide qb's)
    std::uint32_t depth{0};
    Request(std::uint64_t n, std::uint32_t d) noexcept : node(n), depth(d) {}
};
struct Generate : qb::Event {  // GenerateChildrenMessage
    std::uint32_t comp{0};
    explicit Generate(std::uint32_t c) noexcept : comp(c) {}
};
struct UrgentGenerate : qb::Event {  // UrgentGenerateChildrenMessage
    std::uint32_t urgent_child{0};
    std::uint32_t comp{0};
    UrgentGenerate(std::uint32_t u, std::uint32_t c) noexcept : urgent_child(u), comp(c) {}
};
struct UpdateGrant : qb::Event {
    std::uint32_t index{0};
    explicit UpdateGrant(std::uint32_t i) noexcept : index(i) {}
};
struct Traverse : qb::Event {};
struct Report : qb::Event {
    Tally tally;
    explicit Report(const Tally &t) noexcept : tally(t) {}
};

struct Sink {
    Tally tally;
};

// Filled before the engine starts (addActor assigns ids before the actors are constructed) and
// read-only from then on.
struct Field {
    const Plan              *plan{nullptr};
    std::uint32_t            cores{1};
    qb::ActorId              root;
    std::vector<qb::ActorId> planters;  // planters[c] for c >= 1
};

class NodeActor final : public qb::Actor {
    const Field        &_field;
    const qb::ActorId   _parent;
    const std::uint64_t _id;
    const std::uint32_t _depth;
    const std::uint32_t _comp;
    const std::uint32_t _index;  // which child of its parent it is
    const bool          _urgent;
    bool                _granted{false};
    bool                _traversed{false};
    std::uint32_t       _reports{0};
    std::uint64_t       _updated{0};  // the children that sent UpdateGrant, one bit each
    Tally               _tally;       // the node's own counts, then its subtree's
    std::vector<qb::ActorId> _children;

    void generate(std::uint32_t comp, bool urgent, std::uint32_t urgent_child) {
        if (_granted) fail("a second grant at a node");
        _granted = true;
        push<UpdateGrant>(_parent, _index);
        const std::uint32_t B = _field.plan->cfg.binomial;
        _children.reserve(B);
        for (std::uint32_t i = 0; i < B; ++i) {
            const auto child = addRefActor<NodeActor>(std::cref(_field), id(), child_id(_id, i, B),
                                                      _depth + 1, comp, i, urgent && i == urgent_child);
            if (!child.valid())
                qvo::fail("savina/uct qb: addRefActor returned an invalid handle -- the "
                          "VirtualCore's ordinary actor-id pool is exhausted (see "
                          "benchmarks/savina/uct.md)");
            _children.push_back(child.id());
        }
        for (const qb::ActorId c : _children) push<TryGenerate>(c);
    }

    void finish() {
        _tally.own(_id, _depth, _comp, _urgent, _granted);
        push<Report>(_parent, _tally);
        kill();
    }

public:
    NodeActor(const Field &field, qb::ActorId parent, std::uint64_t id, std::uint32_t depth,
              std::uint32_t comp, std::uint32_t index, bool urgent) noexcept
        : _field(field)
        , _parent(parent)
        , _id(id)
        , _depth(depth)
        , _comp(comp)
        , _index(index)
        , _urgent(urgent) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<TryGenerate>(*this);
        registerEvent<Generate>(*this);
        registerEvent<UrgentGenerate>(*this);
        registerEvent<UpdateGrant>(*this);
        registerEvent<Traverse>(*this);
        registerEvent<Report>(*this);
        co_return true;
    }

    void on(TryGenerate const &) {
        ++_tally.messages;
        _tally.work += try_work(_id, _field.plan->cfg.work);
        push<Request>(_field.root, _id, _depth);
    }

    void on(Generate const &event) {
        ++_tally.messages;
        generate(event.comp, false, 0);
    }

    void on(UrgentGenerate const &event) {
        ++_tally.messages;
        generate(event.comp, true, event.urgent_child);
    }

    void on(UpdateGrant const &event) {
        ++_tally.messages;
        const std::uint64_t bit = std::uint64_t{1} << event.index;
        if (_updated & bit) fail("an UpdateGrant repeated");
        _updated |= bit;
        _tally.update_w += child_id(_id, event.index, _field.plan->cfg.binomial);
    }

    void on(Traverse const &) {
        ++_tally.messages;
        if (_traversed) fail("a node traversed twice");
        _traversed = true;
        _tally.work += traverse_work(_id, _comp, _field.plan->cfg.work);
        if (_children.empty()) return finish();
        for (const qb::ActorId c : _children) push<Traverse>(c);
    }

    void on(Report const &event) {
        ++_tally.messages;
        _tally.add(event.tally);
        if (++_reports > _children.size()) fail("a report beyond the node's children");
        if (_reports == _children.size()) finish();
    }
};

// One per core but the root's, created before the window: the only way a qb actor creates an
// actor on another VirtualCore is to ask an actor that lives there.
class PlanterActor final : public qb::Actor {
    const Field        &_field;
    const std::uint32_t _core;

public:
    PlanterActor(const Field &field, std::uint32_t core) noexcept : _field(field), _core(core) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Plant>(*this);
        push<Ready>(_field.root);
        co_return true;
    }

    void on(Plant const &) {
        const std::uint32_t B = _field.plan->cfg.binomial;
        std::vector<qb::ActorId> mine;
        for (std::uint32_t i = _core; i < B; i += _field.cores) {
            const auto child = addRefActor<NodeActor>(std::cref(_field), _field.root, child_id(0, i, B),
                                                      1u, _field.plan->root_comp, i, false);
            if (!child.valid())
                qvo::fail("savina/uct qb: addRefActor returned an invalid handle at a planter");
            mine.push_back(child.id());
        }
        for (const qb::ActorId c : mine) push<TryGenerate>(c);
    }
};

class RootActor final : public qb::Actor {
    const Field             &_field;
    qvo::Watch              &_watch;
    Sink                    &_sink;
    std::uint32_t            _ready{0};
    std::uint64_t            _requests{0};
    std::uint64_t            _grants{0};
    bool                     _complete{false};
    std::uint32_t            _reports{0};
    std::uint64_t            _updated{0};
    Tally                    _tally;
    std::vector<qb::ActorId> _children;

    // Savina's generateTree: the root's own generation, `binomial` children sharing one
    // computation size, every one of them sent TryGenerateChildren.
    void generate_tree() {
        const std::uint32_t B = _field.plan->cfg.binomial;
        _children.assign(B, qb::ActorId{});
        for (std::uint32_t i = 0; i < B; i += _field.cores) {
            const auto child = addRefActor<NodeActor>(std::cref(_field), id(), child_id(0, i, B), 1u,
                                                      _field.plan->root_comp, i, false);
            if (!child.valid())
                qvo::fail("savina/uct qb: addRefActor returned an invalid handle at the root");
            _children[i] = child.id();
        }
        for (std::uint32_t c = 1; c < _field.cores; ++c) push<Plant>(_field.planters[c]);
        for (std::uint32_t i = 0; i < B; i += _field.cores) push<TryGenerate>(_children[i]);
    }

public:
    RootActor(const Field &field, qvo::Watch &watch, Sink &sink) noexcept
        : _field(field), _watch(watch), _sink(sink) {}

    qb::io::async::task<bool> onInit() final {
        registerEvent<Ready>(*this);
        registerEvent<Request>(*this);
        registerEvent<UpdateGrant>(*this);
        registerEvent<Report>(*this);
        push<Ready>(id());
        co_return true;
    }

    // The window opens once every core is running: the root's own Ready and each planter's.
    void on(Ready const &) {
        if (++_ready != _field.cores) return;
        _watch.start();
        generate_tree();
    }

    void on(Request const &event) {
        ++_tally.messages;
        if (_complete) fail("a request after the tree is complete");
        const std::uint32_t B = _field.plan->cfg.binomial;
        if (event.depth == 1) {  // one of the root's own children: where it lives
            if (event.node == 0 || event.node > B) fail("a request from a node the root never created");
            _children[event.node - 1] = event.getSource();
        }
        ++_requests;
        const Grant g = decide(*_field.plan, event.node, event.depth);
        if (g.granted) {
            ++_grants;
            if (g.urgent) push<UrgentGenerate>(event.getSource(), g.urgent_child, g.comp);
            else push<Generate>(event.getSource(), g.comp);
        }
        // Every node asks once and a grant adds `binomial` nodes: once every node there is has
        // asked, the tree is complete and every grant has been acted on -- its children asked.
        if (_requests != B + B * _grants) return;
        _complete = true;
        for (const qb::ActorId c : _children) push<Traverse>(c);
    }

    void on(UpdateGrant const &event) {
        ++_tally.messages;
        const std::uint64_t bit = std::uint64_t{1} << event.index;
        if (_updated & bit) fail("an UpdateGrant repeated at the root");
        _updated |= bit;
        _tally.update_w += child_id(0, event.index, _field.plan->cfg.binomial);
    }

    void on(Report const &event) {
        ++_tally.messages;
        _tally.add(event.tally);
        if (++_reports > _children.size()) fail("a report beyond the root's children");
        if (_reports != _children.size()) return;
        _watch.stop();
        _sink.tally = _tally;
        broadcast<qb::KillEvent>();
    }
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Plan plan  = qvospec::savina::uct::plan(p, false);  // the root's decisions, before the window
    const auto cores = static_cast<int>(p.get("cores"));
    const bool spin  = p.get("wait") != 0;

    Sink  sink;
    Field field;
    field.plan  = &plan;
    field.cores = static_cast<std::uint32_t>(cores < 1 ? 1 : cores);
    {
        qb::Main engine;

        const int ncores = static_cast<int>(field.cores);
        for (int c = 0; c < ncores; ++c) qvoqb::configure_core(engine, c, spin);

        field.root = engine.addActor<RootActor>(0, std::cref(field), std::ref(watch), std::ref(sink));
        field.planters.assign(field.cores, qb::ActorId{});
        for (std::uint32_t c = 1; c < field.cores; ++c)
            field.planters[c] =
                engine.addActor<PlanterActor>(static_cast<qb::CoreId>(c), std::cref(field), c);

        engine.start();
        engine.join();
    }
    // The tally is folded here, outside the window.
    return qvo::Answer{checksum(sink.tally), sink.tally.messages};
}

}  // namespace savina_uct_qb

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::uct::params();
    spec.expected          = qvospec::savina::uct::expected;
    spec.expected_messages = qvospec::savina::uct::expected_messages;
    spec.work_unit         = qvospec::savina::uct::kWorkUnit;
    spec.work_units        = qvospec::savina::uct::work_units;
    spec.idiom_source      = "qb/llm/qb.llm.md: addRefActor<T>() (same-core child, onInit run "
                             "synchronously) + kill(); the fib and fork-join-create adapters";
    spec.idiom_note        = "a granted node creates its children with addRefActor and push<>es "
                             "each a TryGenerate; a node reports its subtree with one push<Report> "
                             "and kills itself; the root's child i lives on VirtualCore i % cores, "
                             "created there by a per-core planter";
    spec.caveats           = qvoqb::caveats();
    spec.caveats.emplace_back(
        "qb has no cross-core spawn: a node lives on its parent's VirtualCore, so with cores=2 the "
        "root's children i with i % 2 == 0 and their subtrees run on core 0, the others on core 1, "
        "and nothing balances the two -- at the defaults 31 145 nodes against 28 845; the pools "
        "place every node dynamically -- see benchmarks/savina/uct.md");
    spec.caveats.emplace_back(
        "the root's children on another core are created by a planter actor of that core on one "
        "Plant event per core (created before the window, not counted in the messages): qb's way "
        "to create an actor on another VirtualCore");
    spec.caveats.emplace_back(
        "every node is alive until the tree's report wave; the cap of 60 000 nodes is the common "
        "comparison cell for every framework: qb 3.2.1's actor id was a 16-bit slot per VirtualCore "
        "(65 534 live actors), which held the default and not Savina's 200 000; since 3.3.0 the id is "
        "64-bit with a 24-bit ordinary slot per VirtualCore (16 711 679), and the canonical 200 000 "
        "verifies as a separate document (--param nodes=200000) -- see benchmarks/savina/uct.md");

    return qvo::run(argc, argv, std::move(spec), savina_uct_qb::body);
}
