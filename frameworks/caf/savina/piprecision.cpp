// @benchmark     savina/piprecision
// @framework     caf 1.1.0
// @idiom-source  the nqueens adapter beside this file (function-based behaviors, stateful_actor,
//                built-in atoms, `self->spawn` from the master's init for its pool, a tick/ok
//                handshake before the window) and CAF's custom type-ID block (caf/type_id.hpp,
//                CAF_BEGIN_TYPE_ID_BLOCK) for the one message element CAF has no built-in ID for.
// @idiom-note    form=0: a work message is `(put_atom, uint32 term)`; an answer is `(add_atom,
//                uint32 term, uint32 worker, uint64 proof, std::vector<uint64> limbs)`, the vector
//                MOVED into the message -- CAF stores it in the message's own allocation and the
//                limbs stay in the buffer the worker filled, which the master reads through a const
//                reference. form=1 (the payload copied into the message's storage) has no CAF
//                counterpart: a CAF message element has a fixed type, and a fixed-size array at
//                the largest term would copy and allocate twenty kilobytes for every one-limb term
//                -- not applicable. Where a worker RUNS is the work-stealing pool's decision.

#include <qvospec/savina/piprecision.h>

#include "../caf_support.h"

#include <caf/actor.hpp>
#include <caf/actor_cast.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/stateful_actor.hpp>
#include <caf/type_id.hpp>

#include <utility>
#include <vector>

// An answer's limbs: std::vector<std::uint64_t> has no built-in CAF type ID (caf/type_id.hpp lists
// the core module's), so it gets one here, at caf::first_custom_type_id -- CAF's own mechanism.
CAF_BEGIN_TYPE_ID_BLOCK(qvo_piprecision, caf::first_custom_type_id)
CAF_ADD_TYPE_ID(qvo_piprecision, (std::vector<std::uint64_t>))
CAF_END_TYPE_ID_BLOCK(qvo_piprecision)

namespace savina_piprecision_caf {

using namespace qvospec::savina::piprecision;

struct worker_state {
    Scratch scratch;
};

// The worker's count goes straight to its Tally, a 128-byte slot of its own: a CAF actor is not
// bound to a thread, so its state has no line that is its thread's alone.
caf::behavior worker_fun(caf::stateful_actor<worker_state> *self, caf::actor master,
                         const Context *context, std::uint32_t index, Tally *tally) {
    return {
        // Handshake, once, outside the window: the worker holds its behavior before the first
        // work message is timed.
        [self, master](caf::tick_atom) { self->mail(caf::ok_atom_v).send(master); },
        [self, master, context, index, tally](caf::put_atom, std::uint32_t term) {
            ++tally->works;
            std::vector<Limb> limbs;  // a fresh buffer per answer: it leaves with the message
            bbp_term(*context, term, self->state().scratch, limbs);
            const Limb proof = bbp_proof(self->state().scratch);
            self->mail(caf::add_atom_v, term, index, proof, std::move(limbs)).send(master);
        },
        [self](caf::close_atom) { self->quit(); },
    };
}

struct master_state {
    std::vector<caf::actor> workers;
    Config                  config;
    std::size_t             ready{0};
    bool                    stop{false};
    qvo::Watch             *watch{nullptr};
    Fold                   *fold{nullptr};
};

caf::behavior master_fun(caf::stateful_actor<master_state> *self, Config config,
                         const Context *context, qvo::Watch *watch, Fold *fold,
                         std::vector<Tally> *tallies) {
    auto &st  = self->state();
    st.config = config;
    st.watch  = watch;
    st.fold   = fold;

    const auto me = caf::actor_cast<caf::actor>(self);
    st.workers.reserve(config.workers);
    for (std::uint32_t w = 0; w < config.workers; ++w) {
        st.workers.push_back(
            self->spawn<qvocaf::kSpawnOptions>(worker_fun, me, context, w, &(*tallies)[w]));
        self->mail(caf::tick_atom_v).send(st.workers.back());
    }

    auto hand_out = [self](std::uint32_t worker) {
        auto &s = self->state();
        self->mail(caf::put_atom_v, s.fold->next()).send(s.workers[worker]);
    };

    return {
        // Savina's StartMessage: the first `batch` terms, round-robin.
        [self, hand_out](caf::ok_atom) {
            auto &s = self->state();
            if (++s.ready != s.workers.size()) return;
            s.watch->start();
            for (std::uint32_t t = 0; t < s.config.batch; ++t) hand_out(t % s.config.workers);
        },
        // Savina's Master.process(ResultMessage).
        [self, hand_out](caf::add_atom, std::uint32_t term, std::uint32_t worker, Limb proof,
                         const std::vector<Limb> &limbs) {
            auto &s = self->state();
            if (worker >= s.config.workers)
                qvo::fail("savina/piprecision: an answer names worker " + std::to_string(worker)
                          + ", which does not exist");
            if (s.fold->add(term, limbs.data(), limbs.size(), proof)) s.stop = true;
            if (!s.stop) hand_out(worker);
            if (!s.fold->complete()) return;
            s.watch->stop();
            for (auto &w : s.workers) self->mail(caf::close_atom_v).send(w);
            self->quit();
        },
    };
}

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const Config c     = config(p);
    const auto   cores = static_cast<std::size_t>(p.get("cores"));
    const bool   spin  = p.get("wait") != 0;
    if (inline_payload(p))
        qvo::not_applicable("form=1 carries the payload in the message's own storage, a "
                            "variable-length tail behind the event (qb's allocated_push); a CAF "
                            "message element has a fixed type, so the CAF counterpart is form=0, "
                            "the vector moved into the message");
    qvocaf::refuse_spin_if_detached(spin);
    const Context context = make_context(c.precision);

    Fold               fold(c, context);
    std::vector<Tally> tallies(c.workers);
    {
        caf::actor_system_config cfg;
        qvocaf::configure(cfg, cores, spin);

        caf::actor_system sys{cfg};
        qvocaf::assert_budget(sys, cores);

        sys.spawn<qvocaf::kSpawnOptions>(master_fun, c, &context, &watch, &fold, &tallies);
        sys.await_all_actors_done();
        qvocaf::assert_pins_took();
    }
    // The system's destructor joined its workers: the fold and every tally are final, and reduced
    // here, outside the window.
    qvo::Answer answer{checksum(fold, works(tallies), reference(p).proofs), 0};
    answer.observed[kObservedTerms] = fold.requested;
    return answer;
}

}  // namespace savina_piprecision_caf

int main(int argc, char **argv) {
    caf::init_global_meta_objects<caf::id_block::qvo_piprecision>();
    caf::core::init_global_meta_objects();

    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::piprecision::params();
    spec.params["form"]    = 0;  // form=1 is not applicable here (see body)
    spec.expected          = qvospec::savina::piprecision::expected;
    spec.work_unit         = qvospec::savina::piprecision::kWorkUnit;
    spec.work_units        = qvospec::savina::piprecision::work_units;
    spec.observed_at_least = {{qvospec::savina::piprecision::kObservedTerms,
                               qvospec::savina::piprecision::min_terms}};
    spec.idiom_source      = "the nqueens adapter (function-based behaviors, stateful_actor, "
                             "self->spawn from init, tick/ok handshake) + CAF_BEGIN_TYPE_ID_BLOCK "
                             "for std::vector<uint64_t>";
    spec.idiom_note        = "(put_atom, term) work messages, (add_atom, term, worker, proof, "
                             "limbs) answers with the limbs vector moved into the message; the "
                             "work-stealing pool decides where each worker runs";
    spec.caveats           = qvocaf::caveats(qvocaf::Shape::other);
    spec.caveats.emplace_back(
        "the payload is a std::vector moved into the message (form=0): CAF allocates the message, "
        "the limbs stay in the worker's buffer, and the master reads them where they are; a CAF "
        "worker is not bound to a thread, so where each term is computed is the pool's decision");

    return qvo::run(argc, argv, std::move(spec), savina_piprecision_caf::body);
}
