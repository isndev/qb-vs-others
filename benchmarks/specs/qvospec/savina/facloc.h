// savina/facloc — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are plain arithmetic with no framework linked (FAIRNESS.md section 0). The rules the ping-pong
// spec states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: Online Facility Location, `facloc` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- FacilityLocationConfig.java for the parameters,
// FacilityLocationAkkaActorBenchmark.scala for the semantics. Deviations are recorded in
// benchmarks/savina/facloc.md.

#ifndef QVOSPEC_SAVINA_FACLOC_H
#define QVOSPEC_SAVINA_FACLOC_H

#include <qvo/harness.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qvospec::savina::facloc {

inline constexpr const char *kId = "savina/facloc";

// The shape, Savina's own: a PRODUCER draws `points` customers on a grid_size x grid_size square
// and sends them, one at a time, to the ROOT QUADRANT -- the next one when the root asks for it
// (NextCustomer, after handling the previous one). A quadrant with no children keeps every
// customer it receives (its support customers) and adds the customer's cost -- the distance to the
// nearest of its facilities -- to its total; when the total exceeds the threshold (alpha x F,
// F = sqrt(2) x grid_size) it OPENS its facility, the midpoint of its box: it tells its parent
// (Facility, passed on up to the root), splits its box in four at that midpoint and CREATES four
// child quadrants, each given the support customers inside its box. A quadrant with children
// forwards every customer it receives to the child whose box contains it. After the last customer
// the producer sends RequestExit; it travels down the tree, and every quadrant answers its parent
// once its children have (ConfirmExit, carrying the facilities and customers below it).
//
// What the cell measures is a stream of small messages routed down a tree that grows by splitting
// where the customers are -- creation on demand, by the actor that owns the region -- with one
// customer in flight at the root at a time and every level below it a pipeline stage.
//
// THE FACILITIES ARE THE SAME IN EVERY RUN. A quadrant's customers arrive in the producer's order
// (one channel from the producer to the root, one from each parent to each child, each FIFO), so
// what a quadrant holds and when it splits depends only on that order -- PROVIDED its cost depends
// only on what it holds. Savina's does not: a quadrant also adds to its facilities the ones its
// parent and siblings TELL it about, at whatever point of its customer stream those messages land,
// so which facilities open is a race. Here a quadrant's facilities are its own midpoint and its
// corner ancestors' (Savina's own comment on `localFacilities`), fixed when it is created, and the
// sibling notifications are not sent (facloc.md, "The facilities, for every interleaving"): the
// quadtree, the facilities and every customer's quadrant are the same for every interleaving and
// the spec computes them in advance (reference()).
//
// `points` -- NUM_POINTS, the customers. Savina's own default, 100 000.
// `grid`   -- GRID_SIZE, the square's side. Savina's own default, 500.
// `alpha`  -- ALPHA, the threshold's factor: a quadrant opens once its cost exceeds alpha x F.
//             Savina's own default, 2 (an integer here; Savina parses a double).
// `seed`   -- the customers' stream. Savina declares SEED = 123456 and its actor implementations
//             never apply it (Point's generator is a static, seeded once, shared by every
//             iteration): here the stream is a function of `seed` and the customer's index.
// `cores`  -- worker budget. The frameworks that place put the producer and the root on core 0
//             and the subtree of the root's child k on core k % cores (facloc.md).
// `wait`   -- 1 = spin, 0 = park. See ping-pong.h for why this is a declared axis.
//
// Savina's CUTOFF_DEPTH (3) and its N and C are parsed by its configuration and read by no actor
// implementation of the benchmark; they have no counterpart here.
inline std::map<std::string, long long> params() {
    return {{"points", 100000}, {"grid", 500}, {"alpha", 2}, {"seed", 123456},
            {"cores", 2},       {"wait", 1}};
}

// Coordinates are integers: one unit of the grid is kUnit sub-units, so a 500-wide square is
// [0, 500 x 65 536) on each axis. A distance is the square root of an exact integer (both squares
// below 2^52 when the side is below 2^26, so their sum converts to a double exactly, with or
// without a fused multiply-add), correctly rounded by IEEE 754 on every platform, and a cost is a
// sum of those in the quadrant's own customer order: the same bits everywhere.
inline constexpr std::uint32_t kUnit     = 1u << 16;
inline constexpr long long     kMaxGrid  = 1000;       // side < 2^26
inline constexpr long long     kMaxPoint = 1LL << 24;  // a customer's index fits the floor's message

struct Config {
    std::uint64_t points{0};
    std::uint32_t side{0};  // grid x kUnit
    double        threshold{0};
    std::uint64_t seed{0};
};

inline Config config(const qvo::Params &p) {
    const long long points = p.get("points");
    const long long grid   = p.get("grid");
    const long long alpha  = p.get("alpha");
    const long long seed   = p.get("seed");
    if (points < 1 || points > kMaxPoint || grid < 1 || grid > kMaxGrid || alpha < 1 || alpha > 1000
        || seed < 0)
        qvo::refuse("savina/facloc: need 1 <= points <= 2^24, 1 <= grid <= 1000, "
                    "1 <= alpha <= 1000, seed >= 0 (got points=" + std::to_string(points)
                    + " grid=" + std::to_string(grid) + " alpha=" + std::to_string(alpha)
                    + " seed=" + std::to_string(seed) + ")");
    Config c;
    c.points = static_cast<std::uint64_t>(points);
    c.side   = static_cast<std::uint32_t>(grid) * kUnit;
    // Savina: threshold = ALPHA * F, F = Math.sqrt(2) * GRID_SIZE -- in sub-units.
    c.threshold = static_cast<double>(alpha) * (std::sqrt(2.0) * static_cast<double>(c.side));
    c.seed      = static_cast<std::uint64_t>(seed);
    return c;
}

struct Point {
    std::uint32_t x{0};
    std::uint32_t y{0};
};

// A customer: where, and which one (its index in the producer's order, from 0).
struct Customer {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t index{0};
};

// The customers, drawn before the run (no mix() inside the window): customer i is uniform on the
// square, from `seed` and i -- Savina's Point.random(GRID_SIZE), two uniform draws per customer.
inline std::vector<Customer> customers(const Config &c) {
    std::vector<Customer> out;
    out.reserve(static_cast<std::size_t>(c.points));
    for (std::uint64_t i = 0; i < c.points; ++i) {
        const std::uint64_t hx = qvo::mix(0xfac10c0000000001ULL ^ (c.seed << 1) ^ (i << 32) ^ i);
        const std::uint64_t hy = qvo::mix(hx ^ 0xfac10c0000000002ULL);
        out.push_back({static_cast<std::uint32_t>(((hx >> 32) * c.side) >> 32),
                       static_cast<std::uint32_t>(((hy >> 32) * c.side) >> 32),
                       static_cast<std::uint32_t>(i)});
    }
    return out;
}

inline double distance(const Point &f, std::uint32_t x, std::uint32_t y) noexcept {
    const std::int64_t dx = static_cast<std::int64_t>(f.x) - static_cast<std::int64_t>(x);
    const std::int64_t dy = static_cast<std::int64_t>(f.y) - static_cast<std::int64_t>(y);
    return std::sqrt(static_cast<double>(dx * dx + dy * dy));
}

// A quadrant's region, half-open on every side: [x1, x2) x [y1, y2). Savina's Box is closed, so a
// customer exactly on a split line belongs to two children at the split; with Savina's doubles
// that never happens, and with integers it could, so here every customer has exactly one quadrant.
struct Box {
    std::uint32_t x1{0}, y1{0}, x2{0}, y2{0};

    Point midpoint() const noexcept { return {x1 + (x2 - x1) / 2, y1 + (y2 - y1) / 2}; }

    // Savina's children, in its order: TOP_LEFT, TOP_RIGHT, BOT_LEFT, BOT_RIGHT (0..3), split at
    // the midpoint -- the facility the quadrant opens.
    Box child(unsigned k) const noexcept {
        const Point m = midpoint();
        switch (k) {
        case 0: return {x1, m.y, m.x, y2};
        case 1: return {m.x, m.y, x2, y2};
        case 2: return {x1, y1, m.x, m.y};
        default: return {m.x, y1, x2, m.y};
        }
    }

    // The child whose box contains (x, y) -- Savina's routing loop over childrenBoundaries.
    unsigned route(std::uint32_t x, std::uint32_t y) const noexcept {
        const Point m = midpoint();
        return (y < m.y ? 2u : 0u) + (x >= m.x ? 1u : 0u);
    }

    bool splittable() const noexcept { return x2 - x1 >= 2 && y2 - y1 >= 2; }
};

// A quadrant's identity: the root is 0 and child k of quadrant q is 4 q + k + 1, its position in
// the complete quadtree -- a function of the path, not of the order quadrants were created in.
inline std::uint64_t child_pid(std::uint64_t pid, unsigned k) noexcept { return 4 * pid + k + 1; }

// What a quadrant is created with: its region, its place, its corner ancestors' facilities and
// the parent's support customers inside its region, in the order the parent received them.
struct Seed {
    Box                   box{};
    std::uint32_t         depth{0};
    std::uint32_t         position{0};  // 0..3 under its parent; the root's is irrelevant
    std::uint64_t         pid{0};
    std::vector<Point>    facilities;
    std::vector<Customer> customers;
};

// ---------------------------------------------------------------------------------------------
// The quadrant's kernel -- Savina's QuadrantActor without its messages, shared by every
// implementation so that every framework does exactly the same work per customer: one distance
// per facility of the quadrant (its depth + 1 of them), a sum, a comparison.
// ---------------------------------------------------------------------------------------------

class Quadrant {
public:
    // Savina's constructor: the facilities inherited plus the quadrant's own midpoint, then every
    // initial customer added in order (addCustomer: a cost, no split -- Savina splits only when a
    // customer ARRIVES, so a quadrant created over its threshold splits on its next customer).
    Quadrant(Seed &&seed, double threshold)
        : _box(seed.box)
        , _depth(seed.depth)
        , _position(seed.position)
        , _pid(seed.pid)
        , _threshold(threshold)
        , _facilities(std::move(seed.facilities)) {
        _facilities.push_back(_box.midpoint());
        _support.reserve(seed.customers.size() + 8);
        for (const Customer &c : seed.customers) keep(c);
    }

    // A customer for a quadrant with no children: kept, its cost added. True when the cost now
    // exceeds the threshold -- the caller then splits (Savina's `if (totalCost > threshold)
    // partition()`).
    bool add(const Customer &c) noexcept {
        keep(c);
        return _cost > _threshold;
    }

    // Savina's partition(): the facility opens, the support customers go to the four children
    // whose regions contain them, in the order they arrived, and the quadrant keeps none.
    std::array<Seed, 4> split() {
        if (!_box.splittable())
            qvo::fail("savina/facloc: a quadrant of depth " + std::to_string(_depth)
                      + " must split below one sub-unit -- the parameters ask for a finer grid");
        _open = true;
        std::array<Seed, 4> seeds;
        for (unsigned k = 0; k < 4; ++k) {
            seeds[k].box        = _box.child(k);
            seeds[k].depth      = _depth + 1;
            seeds[k].position   = k;
            seeds[k].pid        = child_pid(_pid, k);
            seeds[k].facilities = _facilities;
        }
        for (const Customer &c : _support) seeds[_box.route(c.x, c.y)].customers.push_back(c);
        _support.clear();
        _support.shrink_to_fit();
        return seeds;
    }

    unsigned route(const Customer &c) const noexcept { return _box.route(c.x, c.y); }

    bool                         open() const noexcept { return _open; }
    std::uint32_t                depth() const noexcept { return _depth; }
    std::uint32_t                position() const noexcept { return _position; }
    std::uint64_t                pid() const noexcept { return _pid; }
    const std::vector<Customer> &support() const noexcept { return _support; }

private:
    void keep(const Customer &c) noexcept {
        double best = distance(_facilities.front(), c.x, c.y);
        for (std::size_t f = 1; f < _facilities.size(); ++f) {
            const double d = distance(_facilities[f], c.x, c.y);
            if (d < best) best = d;
        }
        _cost += best;
        _support.push_back(c);
    }

    Box                   _box;
    std::uint32_t         _depth;
    std::uint32_t         _position;
    std::uint64_t         _pid;
    double                _threshold;
    std::vector<Point>    _facilities;
    std::vector<Customer> _support;
    double                _cost{0};
    bool                  _open{false};
};

// The root's seed: the whole square, no facility inherited, no customer yet.
inline Seed root_seed(const Config &c) {
    Seed s;
    s.box = {0, 0, c.side, c.side};
    return s;
}

// ---------------------------------------------------------------------------------------------
// The answer
// ---------------------------------------------------------------------------------------------

// What a quadrant reports in its ConfirmExit: itself plus its children's reports. Plain wrapping
// sums -- no mix() inside the window (Huly QB-1017); the checksum mixes the root's totals after it.
struct Tally {
    std::uint64_t quadrants{0};   // quadrants
    std::uint64_t facilities{0};  // open facilities: Savina's numFacilities (the root's counts always)
    std::uint64_t fac_w{0};       // identities of the quadrants that split
    std::uint64_t customers{0};   // support customers of the quadrants that did not: Savina's count
    std::uint64_t cust_ids{0};    // their indices
    std::uint64_t cust_w{0};      // (quadrant identity + 1) x (index + 1), over every one of them
    std::uint64_t notif_w{0};     // facility identities received in Facility messages
    std::uint64_t depth_sum{0};   // the quadrants' depths
    std::uint64_t messages{0};    // deliveries counted at the receivers

    void add(const Tally &o) noexcept {
        quadrants += o.quadrants;
        facilities += o.facilities;
        fac_w += o.fac_w;
        customers += o.customers;
        cust_ids += o.cust_ids;
        cust_w += o.cust_w;
        notif_w += o.notif_w;
        depth_sum += o.depth_sum;
        messages += o.messages;
    }

    // What quadrant `q` says about itself when it confirms (Savina's safelyExit: its facility if it
    // opened one -- the root's always, Savina's `childrenFacilities + 1` at the root -- and its
    // support customers).
    void own(const Quadrant &q, bool root) noexcept {
        quadrants += 1;
        depth_sum += q.depth();
        if (q.open() || root) facilities += 1;
        if (q.open()) fac_w += q.pid();
        std::uint64_t ids = 0;
        for (const Customer &c : q.support()) ids += c.index + 1u;
        customers += q.support().size();
        cust_ids += ids - q.support().size();
        cust_w += (q.pid() + 1) * ids;
    }
};

inline std::uint64_t checksum(const Tally &t) noexcept {
    const std::uint64_t field[] = {t.quadrants, t.facilities, t.fac_w,   t.customers,
                                   t.cust_ids,  t.cust_w,     t.notif_w, t.depth_sum};
    std::uint64_t       acc     = 0;
    for (std::size_t j = 0; j < std::size(field); ++j)
        acc += qvo::mix(field[j] ^ (0xfac10c0073756d00ULL + j * 0x9e3779b97f4a7c15ULL));
    return acc;
}

// The reference: Savina's protocol run sequentially, customer after customer, through the same
// kernel -- which is the run every interleaving reproduces (the class comment says why).
struct Reference {
    Tally                      tally{};
    std::uint64_t              split{0};        // quadrants that split (facilities opened)
    std::uint32_t              height{0};       // the deepest quadrant's depth
    std::array<std::uint64_t, 4> under_root{};  // quadrants under each of the root's children
};

inline Reference reference(const qvo::Params &p) {
    const Config                c   = config(p);
    const std::vector<Customer> all = customers(c);

    struct Node {
        Quadrant                     q;
        std::array<std::uint32_t, 4> child{};
        std::uint32_t                top{0};  // which of the root's children this one is under
    };
    std::vector<Node> nodes;
    nodes.push_back({Quadrant(root_seed(c), c.threshold), {}, 0});
    Reference r;
    Tally    &t = r.tally;

    for (const Customer &cu : all) {
        std::uint32_t u = 0;
        ++t.messages;  // the customer at the root
        while (nodes[u].q.open()) {
            u = nodes[u].child[nodes[u].q.route(cu)];
            ++t.messages;  // the customer at the next quadrant down
        }
        if (!nodes[u].q.add(cu)) continue;
        std::array<Seed, 4> seeds = nodes[u].q.split();
        const std::uint32_t depth = nodes[u].q.depth();
        const std::uint64_t pid   = nodes[u].q.pid();
        // The Facility message: received by each of the depth ancestors on its way to the root.
        t.notif_w += depth * pid;
        t.messages += depth;
        for (unsigned k = 0; k < 4; ++k) {
            const std::uint32_t top = u == 0 ? k : nodes[u].top;
            nodes.push_back({Quadrant(std::move(seeds[k]), c.threshold), {}, top});
            nodes[u].child[k] = static_cast<std::uint32_t>(nodes.size() - 1);
        }
    }
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        t.own(nodes[n].q, n == 0);
        if (nodes[n].q.open()) ++r.split;
        if (nodes[n].q.depth() > r.height) r.height = nodes[n].q.depth();
        if (n > 0) ++r.under_root[nodes[n].top];
    }
    // The other deliveries: a NextCustomer per customer at the producer, a RequestExit per
    // quadrant (the root's from the producer), a ConfirmExit per quadrant but the root.
    t.messages += c.points + 2 * t.quadrants - 1;
    return r;
}

// THE VERIFIED ANSWER: the root's total after the exit wave against the reference's.
//
//   * a customer lost below the root, or delivered twice, or to the wrong child, ends in the
//     wrong place or nowhere: the support counts and their identity-weighted sums move, and the
//     splits that customer caused or prevented move the facilities;
//   * a quadrant that skipped a customer's cost splits at another customer, or never: the
//     facilities and the support sums move;
//   * a Facility message lost or repeated on its way to the root moves notif_w;
//   * a customer lost between the producer and the root, a RequestExit or a ConfirmExit lost,
//     leaves the producer or a parent waiting: the run BLOCKS and fails at the harness's deadline;
//   * what the protocol itself sees is a failure at once (fail() below): a customer after
//     RequestExit, a second RequestExit, a fifth ConfirmExit.
//
// So the answer is the quadtree -- which quadrants opened a facility, and which customers each of
// the others holds -- and is the same for every interleaving.
inline std::uint64_t expected(const qvo::Params &p) { return checksum(reference(p).tally); }

[[noreturn]] inline void fail(const char *what) {
    qvo::fail(std::string("savina/facloc: protocol violated -- ") + what);
}

// The report divides by this: one customer -- produced, routed down to its quadrant, costed.
inline constexpr const char *kWorkUnit = "customer";
inline std::uint64_t work_units(const qvo::Params &p) { return config(p).points; }

// Counted at the receivers: every hop of every customer, every NextCustomer, every Facility hop,
// a RequestExit per quadrant and a ConfirmExit per quadrant but the root (reference(), above).
inline std::uint64_t expected_messages(const qvo::Params &p) { return reference(p).tally.messages; }

}  // namespace qvospec::savina::facloc

#endif  // QVOSPEC_SAVINA_FACLOC_H
