// @benchmark     savina/barber
// @framework     baseline  (NOT an actor framework -- the floor)
// @idiom-source  none. The workload on the many-actor floor of ../baseline_support.h: the room,
//                the barber and the factory are fixed ids, and a customer is a heap node in the
//                factory's worker's slot table, allocated inside the window and freed after it
//                has reported.
// @idiom-note    The floor does what the semantics require and nothing else: a customer is one
//                `new`, one slot and one `delete`, the room is a ring of customer ids, and every
//                message is one ring push. Both factory shapes of the spec's `pace` axis: pace=0
//                produces every customer inside the one Start handler, as the reference does;
//                pace=1 one per self-addressed Start. A worker inside the pace=0 loop drains none
//                of its inbound rings, so at cores>=2 that shape is refused above `haircuts` =
//                65 536, the size of one ring (see body()). With cores=2 the factory and the
//                customers are on worker 0 and the room and the barber on worker 1, the placement
//                qb's cell has.

#include <qvospec/savina/barber.h>

#include "../baseline_support.h"

#include <type_traits>
#include <vector>

namespace savina_barber_baseline {

using namespace qvospec::savina::barber;

// kEnter: a = customer, b = its number. kReturned: a = customer, b = its number. kNext: a = the
// number of the customer just served, 0 for the room's own wake-up. kDone: a = value, b = messages
// (the barber's Done carries the haircut in a). kExit: a = partial checksum, b = messages.
enum Tag : std::uint32_t { kStart = 1, kEnter, kFull, kWait, kNext, kReturned, kDone, kExit };

struct Customer {
    std::uint64_t number{0};
    std::uint64_t starts{0};
    std::uint64_t waits{0};
    std::uint64_t fulls{0};
    std::uint64_t received{0};
};

// One worker's actors: the fixed ones hold a null slot, a customer its node. Touched only by the
// thread owning the worker (and by the main thread before start).
struct Table {
    std::vector<Customer *>    slots;
    std::vector<std::uint32_t> free_slots;

    std::uint32_t alloc(unsigned w, unsigned W, Customer *node) {
        std::uint32_t slot;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(slots.size());
            slots.push_back(nullptr);
        }
        slots[slot] = node;
        return slot * W + w;
    }

    void release(std::uint32_t slot) {
        delete slots[slot];
        slots[slot] = nullptr;
        free_slots.push_back(slot);
    }
};

struct FactoryState {
    std::uint64_t produced{0};
    std::uint64_t served{0};
    std::uint64_t received{0};
    std::uint64_t messages{0};  // reported by the customers
    std::uint64_t sum{0};
};

struct Seat {
    std::uint32_t customer{0};
    std::uint64_t number{0};
};

struct RoomState {
    std::vector<Seat> seats;
    std::size_t       head{0};
    std::size_t       waiting{0};
    bool              asleep{true};
    std::uint64_t     acc{0};  // the room's terms of the checksum (barber.h)
    std::uint64_t     rejected{0};
    std::uint64_t     wakeups{0};
    std::uint64_t     wake_nexts{0};
    std::uint64_t     barber_nexts{0};
    std::uint64_t     received{0};
    bool                       exit{false};
    std::uint64_t              exit_partial{0};
    std::uint64_t              exit_messages{0};
};

struct BarberState {
    std::uint64_t haircuts{0};
    std::uint64_t acc{0};  // the barber's terms of the checksum (barber.h)
    std::uint64_t received{0};
};

qvo::Answer body(const qvo::Params &p, qvo::Watch &watch) {
    const auto     haircuts = static_cast<std::uint64_t>(p.get("haircuts"));
    const auto     seats    = static_cast<std::size_t>(p.get("room"));
    const auto     apr      = static_cast<std::uint64_t>(p.get("apr"));
    const auto     ahr      = static_cast<std::uint64_t>(p.get("ahr"));
    const bool     pace     = paced(p);
    const auto     cores    = static_cast<unsigned>(p.get("cores"));
    const bool     spin     = p.get("wait") != 0;
    const unsigned W        = cores < 1 ? 1u : cores;
    const unsigned shop     = 1 % W;

    // The pace=0 factory sends every Enter from inside one handler, during which worker 0 drains
    // none of its inbound rings. With one worker that is safe (Mesh::send drains a full ring it
    // owns inline). With two, worker 1 can fill the ring back to worker 0 (Wait, Start and Done
    // for every customer it handles) and then spin on it; the loop still finishes as long as the
    // Enters it sends all fit in the ring to worker 1 -- `haircuts` <= one ring. Above that the two
    // workers can each spin on the other's full ring, so that configuration is refused here
    // rather than allowed to hang. (kOneRing is held to the mesh's own capacity below.)
    constexpr std::uint64_t kOneRing = std::uint64_t{1} << 16;
    if (!pace && W > 1 && haircuts > kOneRing)
        qvo::not_applicable("pace=0 at cores>=2 with haircuts above 65 536: the floor's looping "
                            "factory drains none of its inbound rings, and its Enters no longer "
                            "fit in the ring to the room's worker -- the two workers could each "
                            "spin on the other's full ring");

    std::vector<Table>  tables(W);
    const std::uint32_t factory = tables[0].alloc(0, W, nullptr);
    const std::uint32_t room    = tables[shop].alloc(shop, W, nullptr);
    const std::uint32_t barber  = tables[shop].alloc(shop, W, nullptr);

    FactoryState f;
    RoomState    r;
    BarberState  b;
    r.seats.resize(seats);

    std::uint64_t checksum = 0;
    std::uint64_t messages = 0;

    qvobase::Mesh mesh(W, spin, [&](auto &m, unsigned worker, const qvobase::Msg &msg) {
        auto node = [&](std::uint32_t id) -> Customer & { return *tables[worker].slots[id / W]; };

        // The room's Exit leaves only once the barber's n-th Next is in: the factory's Exit and
        // the barber's last Next come from two senders, and nothing orders them (an (n+1)-th
        // fail()s in kNext).
        auto room_exit = [&] {
            if (!r.exit || r.barber_nexts < haircuts) return;
            r.exit             = false;
            const auto partial = r.exit_partial + r.acc + weight(kTagWake) * r.wake_nexts -
                                 weight(kTagWake) * r.wakeups;
            m.send(worker, qvobase::Msg{barber, kExit, partial, r.exit_messages + r.received});
        };

        switch (msg.tag) {
        case kStart: {
            if (msg.dst == factory) {
                // pace=0: every customer from this one handler, as the reference; pace=1: one.
                ++f.received;
                do {
                    const std::uint64_t i  = f.produced++;
                    auto               *c  = new Customer{i + 1, 0, 0, 0, 0};
                    const std::uint32_t id = tables[worker].alloc(worker, W, c);
                    m.send(worker, qvobase::Msg{room, kEnter, id, i + 1});
                    f.sum += production_work(i, apr);
                } while (!pace && f.produced < haircuts);
                if (pace && f.produced < haircuts)
                    m.send(worker, qvobase::Msg{factory, kStart, 0, 0});
            } else {
                Customer &c = node(msg.dst);
                ++c.received;
                ++c.starts;
            }
            break;
        }
        case kEnter: {
            const auto          customer = static_cast<std::uint32_t>(msg.a);
            const std::uint64_t id_i     = identity(msg.b);
            if (msg.dst == room) {
                ++r.received;
                if (r.waiting == r.seats.size()) {
                    ++r.rejected;
                    r.acc -= (weight(kTagFull) + weight(kTagReturned)) * id_i;
                    m.send(worker, qvobase::Msg{customer, kFull, 0, 0});
                    break;
                }
                r.acc += weight(kTagEnter) * id_i;
                r.seats[(r.head + r.waiting++) % r.seats.size()] = Seat{customer, msg.b};
                if (r.asleep) {
                    r.asleep = false;
                    ++r.wakeups;
                    r.acc += weight(kTagWait) * id_i;
                    m.send(worker, qvobase::Msg{room, kNext, 0, 0});
                } else {
                    m.send(worker, qvobase::Msg{customer, kWait, 0, 0});
                }
            } else {  // the barber
                ++b.received;
                b.acc += weight(kTagCut) * id_i;
                const std::uint64_t k = b.haircuts++;
                m.send(worker, qvobase::Msg{customer, kStart, 0, 0});
                const std::uint64_t h = haircut_work(k, ahr);
                m.send(worker, qvobase::Msg{customer, kDone, h, 0});
                m.send(worker, qvobase::Msg{room, kNext, msg.b, 0});
            }
            break;
        }
        case kFull: {
            Customer &c = node(msg.dst);
            ++c.received;
            ++c.fulls;
            m.send(worker, qvobase::Msg{factory, kReturned, msg.dst, c.number});
            break;
        }
        case kWait: {
            if (msg.dst == barber) {
                ++b.received;
                b.acc += weight(kTagNap);
            } else {
                Customer &c = node(msg.dst);
                ++c.received;
                ++c.waits;
            }
            break;
        }
        case kNext: {
            ++r.received;
            if (msg.a == 0) {
                ++r.wake_nexts;
            } else {
                if (++r.barber_nexts > haircuts) fail("the room received more Nexts than haircuts");
                r.acc += weight(kTagNext) * identity(msg.a);
            }
            if (r.waiting != 0) {
                const Seat seat = r.seats[r.head];
                r.head          = (r.head + 1) % r.seats.size();
                --r.waiting;
                m.send(worker, qvobase::Msg{barber, kEnter, seat.customer, seat.number});
            } else {
                r.acc -= weight(kTagNap);
                r.asleep = true;
                m.send(worker, qvobase::Msg{barber, kWait, 0, 0});
            }
            room_exit();
            break;
        }
        case kReturned: {
            ++f.received;
            f.sum += weight(kTagReturned) * identity(msg.b);
            m.send(worker, qvobase::Msg{room, kEnter, msg.a, msg.b});
            break;
        }
        case kDone: {
            if (msg.dst == factory) {
                ++f.received;
                f.sum += msg.a;
                f.messages += msg.b;
                if (++f.served == haircuts)
                    m.send(worker, qvobase::Msg{room, kExit, f.sum, f.messages + f.received});
            } else {
                Customer           &c     = node(msg.dst);
                const std::uint64_t value =
                    qvo::mix(c.number) + msg.a +
                    identity(c.number) * (weight(kTagStart) * c.starts +
                                          weight(kTagWait) * c.waits + weight(kTagFull) * c.fulls);
                const std::uint64_t received = c.received + 1;
                tables[worker].release(msg.dst / W);
                m.send(worker, qvobase::Msg{factory, kDone, value, received});
            }
            break;
        }
        case kExit: {
            if (msg.dst == room) {
                ++r.received;
                r.exit          = true;
                r.exit_partial  = msg.a;
                r.exit_messages = msg.b;
                room_exit();
            } else {  // the barber: the end of the chain
                ++b.received;
                checksum = msg.a + b.acc;
                messages = msg.b + b.received;
                watch.stop();
                m.stop();
            }
            break;
        }
        }
    });
    static_assert(std::remove_reference_t<decltype(mesh)>::kRingCapacity == kOneRing,
                  "the pace=0 refusal above is sized on one ring of the mesh");
    mesh.start();

    watch.start();
    mesh.send(0, qvobase::Msg{factory, kStart, 0, 0});
    mesh.run();

    // The room's counts are final: its worker has stopped, and run() returned on this thread.
    qvo::Answer answer{checksum, messages};
    answer.observed[kObservedRejections] = r.rejected;
    answer.observed[kObservedWakeups]    = r.wakeups;
    return answer;
}

}  // namespace savina_barber_baseline

int main(int argc, char **argv) {
    qvo::Spec spec;
    spec.benchmark         = QVO_BENCHMARK_ID;
    spec.framework         = QVO_FRAMEWORK_ID;
    spec.framework_version = QVO_FRAMEWORK_VERSION;
    spec.params            = qvospec::savina::barber::params();
    // The factory shape this floor's cell runs: the reference's (pace=0) until the quiet-host
    // measurement of both forms names the faster one (benchmarks/savina/barber.md).
    spec.params["pace"] = 0;
    spec.expected          = qvospec::savina::barber::expected;
    spec.work_unit         = qvospec::savina::barber::kWorkUnit;
    spec.work_units        = qvospec::savina::barber::work_units;
    spec.observed_at_least[qvospec::savina::barber::kObservedWakeups] =
        qvospec::savina::barber::min_wakeups;
    spec.idiom_source      = "none -- hand-written floor";
    spec.idiom_note        = "raw pinned threads + one bounded SPSC ring per (worker, worker) "
                             "pair; a customer is a heap node in the factory's worker's slot table, "
                             "created inside the window and freed after reporting; pace=0: every "
                             "customer from one Start handler, pace=1: one per self-addressed "
                             "Start; not an actor framework";
    spec.caveats           = {
        "THIS IS NOT A FRAMEWORK. A customer is one `new`, one slot and one `delete`, the waiting "
        "room a ring of ids, a message one ring push: the floor for what this coordination costs",
        "the factory and every customer are on worker 0, the room and the barber on worker "
        "1 % cores -- the static placement qb's cell has, so this floor bounds the placing "
        "frameworks and NOT the pools",
        "the run's `pace` is in its params: 0 is the reference's factory, every customer from "
        "one handler, refused at cores>=2 above haircuts=65 536 (one ring: a worker inside the "
        "loop drains none of its inbound rings); 1 adds n-1 self-addressed Starts to the "
        "reported count",
        "wait=1 busy-polls the rings; wait=0 parks an idle worker on a condition variable"};

    return qvo::run(argc, argv, std::move(spec), savina_barber_baseline::body);
}
