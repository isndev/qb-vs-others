// savina/apsp — the shared, framework-free specification.
//
// Every framework's implementation includes THIS header; the expected checksum and message count
// are computed with no framework linked (FAIRNESS.md section 0). The rules the ping-pong spec
// states about the checksum (a WRAPPING SUM of mix(), never an XOR) apply unchanged.
//
// Savina reference: All-Pairs Shortest Path, `apsp` (Imam & Sarkar, AGERE 2014), of the
// "parallelism" group -- ApspConfig.java for the parameters, ApspUtils.scala for the graph,
// ApspAkkaActorBenchmark.scala for the semantics. Deviations are recorded in
// benchmarks/savina/apsp.md.

#ifndef QVOSPEC_SAVINA_APSP_H
#define QVOSPEC_SAVINA_APSP_H

#include <qvo/harness.h>

#include <cstddef>
#include <cstdint>
#ifdef QVO_REAL_ADAPTER_CONTROLS
#include <cstdio>
#include <cstdlib>
#endif
#include <memory>
#include <string>
#include <vector>

namespace qvospec::savina::apsp {

inline constexpr const char *kId = "savina/apsp";

// The shape, Savina's own: Floyd-Warshall over a graph of `nodes` vertices, cut into a grid of
// (nodes / block)^2 BLOCK ACTORS, each owning a block x block tile of the distance matrix. A block
// computes generation g + 1 of its tile from generation g of the whole matrix -- pivot k = g:
//
//     next[i][j] = min(prev[i][j], D[i][k] + D[k][j])
//
// D[i][k] lies in the block of its own block-ROW that holds column k, D[k][j] in the block of its
// own block-COLUMN that holds row k. So every block sends each generation it computes to the
// 2 * (nodes / block - 1) blocks of its row and of its column -- all of them, every generation,
// although only the two of the pivot's block-row and block-column read it (Savina's own
// comment: "this is inefficient version where data is sent to neighbors who might not need it")
// -- and computes generation g + 1 only once it holds generation g from every one of them. The
// phases are synchronised by the data itself: no barrier, no coordinator, and no block can run
// more than ONE generation ahead of a neighbour (it needs that neighbour's previous generation).
// After `nodes` generations every tile holds its part of the shortest-path matrix.
//
// It is the suite's computation-over-a-grid shape: each phase of a block is block^2 min-plus
// steps (2 500 at the defaults) BEFORE its ten sends, so a handler is long and the messages are
// few -- the cell asks what a framework adds between a block becoming ready and its successors
// running, not what a message costs (benchmarks/savina/apsp.md, "What it can and cannot show").
//
// `nodes`  -- vertices. Savina's own default, 300 (ApspConfig.N).
// `block`  -- the tile's edge. Savina's own default, 50 (ApspConfig.B): a 6 x 6 grid of 36 blocks,
//             each with 10 neighbours.
// `weight` -- the largest edge weight. Savina's own default, 100 (ApspConfig.W).
// `form`   -- how a qb block publishes a generation, a declared axis (apsp.md, "The publication"):
//             0 = `push` (published by the core's next flush, after the handlers of its pass),
//             1 = `send` (handed to the peer's ring at once). The three other adapters publish at
//             once by construction and declare form=1 not applicable.
// `cores`  -- the worker budget. Where a framework places, block-row bi lives on core
//             bi * cores / (nodes / block): contiguous bands of block-rows, so every core holds
//             whole rows and a block's column neighbours straddle the cores.
// `wait`   -- 1 = spin, 0 = park.
inline std::map<std::string, long long> params() {
    return {{"nodes", 300}, {"block", 50}, {"weight", 100}, {"form", 0}, {"cores", 2}, {"wait", 1}};
}

// The parameters, checked before anything is built (qvo::refuse, exit 2): a grid of at least 2 x 2
// blocks -- a single block has no neighbour, waits for nothing and never starts, which is also
// what Savina's would do -- tiles that cover the matrix exactly (Savina silently drops the
// remainder of nodes / block), a weight its generator can draw, and sizes the 64-bit sums and
// the O(nodes^3) reference stay well inside.
inline constexpr std::uint32_t kMaxNodes  = 2048;
inline constexpr std::uint32_t kMaxBlocks = 128;  // per dimension: 16 384 block actors
inline constexpr std::uint32_t kMaxWeight = 65536;

struct Shape {
    std::uint32_t nodes{0};
    std::uint32_t block{0};
    std::uint32_t weight{0};
    std::uint32_t grid{0};       // blocks per dimension
    std::uint32_t blocks{0};     // grid^2
    std::uint32_t neighbours{0}; // 2 * (grid - 1)
    std::uint32_t form{0};
    std::uint32_t cores{0};
};

inline Shape shape(const qvo::Params &p) {
    const long long nodes  = p.get("nodes");
    const long long block  = p.get("block");
    const long long weight = p.get("weight");
    const long long form   = p.get("form");
    const long long cores  = p.get("cores");
    const bool      sizes  = nodes >= 2 && nodes <= kMaxNodes && block >= 1 && nodes % block == 0
                       && nodes / block >= 2 && nodes / block <= kMaxBlocks;
    if (!sizes || weight < 1 || weight > kMaxWeight || (form != 0 && form != 1) || cores < 1
        || cores > nodes / (block < 1 ? 1 : block))
        qvo::refuse("savina/apsp: need 2 <= nodes <= " + std::to_string(kMaxNodes)
                    + ", block >= 1 dividing nodes with 2 <= nodes / block <= "
                    + std::to_string(kMaxBlocks) + ", 1 <= weight <= " + std::to_string(kMaxWeight)
                    + ", form 0 or 1, 1 <= cores <= nodes / block (got nodes="
                    + std::to_string(nodes) + " block=" + std::to_string(block) + " weight="
                    + std::to_string(weight) + " form=" + std::to_string(form)
                    + " cores=" + std::to_string(cores) + ")");
    Shape s;
    s.nodes      = static_cast<std::uint32_t>(nodes);
    s.block      = static_cast<std::uint32_t>(block);
    s.weight     = static_cast<std::uint32_t>(weight);
    s.grid       = s.nodes / s.block;
    s.blocks     = s.grid * s.grid;
    s.neighbours = 2 * (s.grid - 1);
    s.form       = static_cast<std::uint32_t>(form);
    s.cores      = static_cast<std::uint32_t>(cores);
    return s;
}

// Where a framework that places puts block `id`: contiguous bands of block-rows.
inline std::uint32_t core_of(const Shape &s, std::uint32_t id) noexcept {
    return (id / s.grid) * s.cores / s.grid;
}

// Savina's graph, bit for bit (ApspUtils.generateGraph): a COMPLETE undirected graph, the weight
// of edge (i, j), i < j, drawn in row-major order from Savina's own PseudoRandom seeded with
// `nodes` -- value = (value * 1309 + 13849) & 65535, nextInt(W) = value % W -- plus one; 0 on the
// diagonal. Generated outside the window.
inline std::vector<std::int64_t> graph(std::uint32_t nodes, std::uint32_t weight) {
    std::vector<std::int64_t> d(static_cast<std::size_t>(nodes) * nodes, 0);
    std::uint64_t             value = nodes;
    for (std::uint32_t i = 0; i < nodes; ++i)
        for (std::uint32_t j = i + 1; j < nodes; ++j) {
            value                                       = (value * 1309 + 13849) & 65535;
            const auto r                                = static_cast<std::int64_t>(value % weight) + 1;
            d[static_cast<std::size_t>(i) * nodes + j] = r;
            d[static_cast<std::size_t>(j) * nodes + i] = r;
        }
    return d;
}

// ---------------------------------------------------------------------------------------------
// The blocks' state, framework-free: one region per block in memory the body owns
// ---------------------------------------------------------------------------------------------
//
// Savina allocates a FRESH tile per generation and shares it by reference (the JVM collects the
// old ones). Here a block owns TWO tiles and publishes a pointer to the one it just computed: when
// it computes generation g + 1 into the tile that held generation g - 1, every neighbour has
// already sent it generation g -- so every neighbour has computed its own generation g, the only
// computation that reads this block's generation g - 1, and holds no pointer to it any more. No
// allocation, no reference count, no free inside the window, for every framework alike: the cell
// measures the phases and the computation, not each framework's allocator (apsp.md, deviations).
// Every message that hands a tile over is a release/acquire edge in every framework here, so the
// reads of a neighbour happen-before the overwrite.
//
// Each block's region -- its State, its inbox (the tile pointers of the two generations it can be
// receiving, one slot per neighbour) and its two tiles -- starts on a 128-byte boundary and is a
// whole number of 128-byte lines (thread_ring::Tally says why 128 and not 64): only the thread
// running the block writes it, and no two blocks of two threads share a line or a prefetched pair.
inline constexpr std::size_t kLine = 128;
struct alignas(kLine) Line {
    unsigned char bytes[kLine];
};

struct alignas(kLine) State {
    std::uint32_t generation{0};  // generations computed, 0 .. nodes
    std::uint32_t pending[2]{};   // tiles held for the generation of each parity
    std::uint64_t received{0};    // data messages accepted
    std::uint64_t ahead{0};       // ... of which one generation ahead of the block's own
    std::uint64_t initial{0};     // Initial messages received
    std::uint64_t work{0};        // the witness of every phase computed (witness() below)
};
static_assert(sizeof(State) == kLine, "a block's State is one 128-byte line");

class Storage {
public:
    // Every block's generation 0 is its tile of Savina's graph.
    explicit Storage(const Shape &s)
        : shape_(s)
        , inbox_lines_(lines_for(2u * s.neighbours * sizeof(const std::int64_t *)))
        , tile_lines_(lines_for(static_cast<std::size_t>(s.block) * s.block * sizeof(std::int64_t)))
        , stride_(1 + inbox_lines_ + 2 * tile_lines_)
        , lines_(new Line[stride_ * s.blocks]()) {
        const std::vector<std::int64_t> g = graph(s.nodes, s.weight);
        for (std::uint32_t b = 0; b < s.blocks; ++b) {
            new (&state(b)) State{};
            std::int64_t     *t  = tile(b, 0);
            const std::size_t r0 = static_cast<std::size_t>(b / s.grid) * s.block;
            const std::size_t c0 = static_cast<std::size_t>(b % s.grid) * s.block;
            for (std::size_t i = 0; i < s.block; ++i)
                for (std::size_t j = 0; j < s.block; ++j)
                    t[i * s.block + j] = g[(r0 + i) * s.nodes + c0 + j];
        }
    }

    const Shape &shape() const noexcept { return shape_; }
    State       &state(std::uint32_t b) noexcept { return *reinterpret_cast<State *>(region(b)); }
    const State &state(std::uint32_t b) const noexcept {
        return *reinterpret_cast<const State *>(region(b));
    }
    const std::int64_t **inbox(std::uint32_t b) noexcept {
        return reinterpret_cast<const std::int64_t **>(region(b) + 1);
    }
    std::int64_t *tile(std::uint32_t b, std::uint32_t parity) noexcept {
        return reinterpret_cast<std::int64_t *>(region(b) + 1 + inbox_lines_ + parity * tile_lines_);
    }
    const std::int64_t *tile(std::uint32_t b, std::uint32_t parity) const noexcept {
        return reinterpret_cast<const std::int64_t *>(region(b) + 1 + inbox_lines_
                                                      + parity * tile_lines_);
    }

private:
    static std::size_t lines_for(std::size_t bytes) noexcept { return (bytes + kLine - 1) / kLine; }
    Line       *region(std::uint32_t b) noexcept { return lines_.get() + stride_ * b; }
    const Line *region(std::uint32_t b) const noexcept { return lines_.get() + stride_ * b; }

    Shape                   shape_;
    std::size_t             inbox_lines_;
    std::size_t             tile_lines_;
    std::size_t             stride_;
    std::unique_ptr<Line[]> lines_;
};

// The min-plus step of one generation, the same object code for every framework: `row` is the
// tile of this block-row holding column k, `col` the tile of this block-column holding row k
// (either may be `prev` itself), `kl` = k % block. Returns the sum of the new tile's distances,
// the phase's term in the block's witness (below) -- one add per distance, the only work the cell
// adds to Savina's.
inline std::uint64_t relax(const std::int64_t *prev, const std::int64_t *row,
                           const std::int64_t *col, std::uint32_t kl, std::uint32_t block,
                           std::int64_t *next) noexcept {
    const std::int64_t *ck  = col + static_cast<std::size_t>(kl) * block;
    std::uint64_t       sum = 0;
    for (std::uint32_t i = 0; i < block; ++i) {
        const std::int64_t  dik = row[static_cast<std::size_t>(i) * block + kl];
        const std::int64_t *p   = prev + static_cast<std::size_t>(i) * block;
        std::int64_t       *n   = next + static_cast<std::size_t>(i) * block;
        for (std::uint32_t j = 0; j < block; ++j) {
            const std::int64_t via = dik + ck[j];
            n[j]                   = via < p[j] ? via : p[j];
            sum += static_cast<std::uint64_t>(n[j]);
        }
    }
    return sum;
}

// The WITNESS of a block's phases: after each phase, witness = witness * kWitness + the sum of the
// tile it computed (wrapping). The final tile alone does not prove the phases ran right -- on a
// complete graph Floyd-Warshall converges long before its last pivot and through many tied paths,
// so a phase whose improvements were thrown away still ends on the right matrix (the negative
// control's skip-change plant: 282 distances discarded, the final matrix unchanged) -- but its
// tile's sum, that phase and the ones after it, is not the reference's. Order-sensitive across
// phases, so two wrong phases cannot cancel.
inline constexpr std::uint64_t kWitness = 0x100000001b3ULL;
inline std::uint64_t witness(std::uint64_t acc, std::uint64_t phase_sum) noexcept {
    return acc * kWitness + phase_sum;
}

// A protocol violation the checksum could not see in time -- a tile delivered twice, from a block
// that is not a neighbour, for a generation the block cannot be at, or a phase computed before
// every neighbour's tile was in -- stops here, through qvo::fail() (exit 1 and a document, from
// the thread that saw it; never std::abort -- harness.h).
[[noreturn]] inline void fail(const std::string &what) {
    qvo::fail("savina/apsp: protocol violated -- " + what);
}

// One block, as every adapter's actor drives it. Constant once built; everything it changes is in
// its region of the Storage.
//
//   on Initial:  publish(0, on_initial())                      -- Savina's ApspInitialMessage
//   on a tile:   if (accept(g, from, tile))
//                    do { tile = compute(); if (publishes()) publish(generation(), tile); }
//                    while (ready());
//                if (finished()) Done to the sink
//
// where publish(g, tile) sends (g, id(), tile) to every neighbour, in for_each_neighbour's order.
class Block {
public:
    Block() = default;
    Block(Storage &storage, std::uint32_t id) noexcept
        : shape_(storage.shape())
        , id_(id)
        , bi_(id / shape_.grid)
        , bj_(id % shape_.grid)
        , state_(&storage.state(id))
        , inbox_(storage.inbox(id))
        , tile_{storage.tile(id, 0), storage.tile(id, 1)} {}

    std::uint32_t id() const noexcept { return id_; }

    // Savina's neighbour list, in its order: the blocks of this block-COLUMN, then of its ROW.
    template <typename F>
    void for_each_neighbour(F &&f) const {
        for (std::uint32_t r = 0; r < shape_.grid; ++r)
            if (r != bi_) f(r * shape_.grid + bj_);
        for (std::uint32_t c = 0; c < shape_.grid; ++c)
            if (c != bj_) f(bi_ * shape_.grid + c);
    }

    // Savina's ApspInitialMessage: the tile to publish as generation 0.
    const std::int64_t *on_initial() {
        if (state_->initial++ != 0) fail("block " + std::to_string(id_) + " received a second Initial");
        return tile_[0];
    }

    // A neighbour's tile of generation `g`. Returns true when the inbox of the block's own
    // generation is complete -- the caller then calls compute() while ready() says so.
    bool accept(std::uint32_t g, std::uint32_t from, const std::int64_t *data) {
        State              &st  = *state_;
        const std::uint32_t own = st.generation;
        if (own == shape_.nodes)
            fail("block " + std::to_string(id_) + " received generation " + std::to_string(g)
                 + " from block " + std::to_string(from) + " after its last phase");
        std::uint32_t parity;
        if (g == own) {
            parity = own & 1u;
        } else if (g == own + 1) {
            parity = g & 1u;
            ++st.ahead;
        } else {
            fail("block " + std::to_string(id_) + " at generation " + std::to_string(own)
                 + " received generation " + std::to_string(g) + " from block "
                 + std::to_string(from) + " -- a neighbour can be one generation ahead, never more"
                 + (g < own ? ", and a phase that ended without it ended early" : ""));
        }
        const std::uint32_t slot = slot_of(from);
        if (slot == kNoSlot)
            fail("block " + std::to_string(id_) + " received a tile from block "
                 + std::to_string(from) + ", which is not in its row or its column");
        const std::int64_t *&held = inbox_[parity * shape_.neighbours + slot];
        if (held != nullptr)
            fail("block " + std::to_string(id_) + " received generation " + std::to_string(g)
                 + " from block " + std::to_string(from) + " twice");
        held = data;
        ++st.received;
        return ++st.pending[parity] == shape_.neighbours && g == own;
    }

    bool ready() const noexcept {
        const State &st = *state_;
        return st.generation < shape_.nodes && st.pending[st.generation & 1u] == shape_.neighbours;
    }

    // Generation `generation() + 1`, from every neighbour's tile of `generation()`. Returns the
    // new tile. A call with the phase's inbox short is a phase that ends early: refused.
    const std::int64_t *compute() {
        State              &st  = *state_;
        const std::uint32_t own = st.generation;
        const std::uint32_t par = own & 1u;
        if (own >= shape_.nodes || st.pending[par] != shape_.neighbours)
            fail("block " + std::to_string(id_) + " computed generation " + std::to_string(own + 1)
                 + " holding " + std::to_string(st.pending[par]) + " of its "
                 + std::to_string(shape_.neighbours) + " tiles of generation "
                 + std::to_string(own) + " -- a phase that ends early");
        const std::int64_t **held = inbox_ + par * shape_.neighbours;
        const std::uint32_t  kb   = own / shape_.block;
        const std::int64_t  *prev = tile_[par];
        const std::int64_t  *row  = kb == bj_ ? prev : held[row_slot(kb)];
        const std::int64_t  *col  = kb == bi_ ? prev : held[col_slot(kb)];
        std::int64_t        *next = tile_[par ^ 1u];
        st.work = witness(st.work, relax(prev, row, col, own % shape_.block, shape_.block, next));
        for (std::uint32_t s = 0; s < shape_.neighbours; ++s) held[s] = nullptr;
        st.pending[par] = 0;
        st.generation   = own + 1;
        return next;
    }

    std::uint32_t generation() const noexcept { return state_->generation; }
    // Generation `nodes` is the answer, and nobody needs it: Savina sends it to blocks that have
    // already exited (dead letters); here it is not sent (apsp.md, deviations).
    bool publishes() const noexcept { return state_->generation < shape_.nodes; }
    bool finished() const noexcept { return state_->generation == shape_.nodes; }

private:
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;

    // Column neighbours take slots 0 .. grid-2 (by block-row), row neighbours grid-1 .. 2grid-3.
    std::uint32_t col_slot(std::uint32_t r) const noexcept { return r < bi_ ? r : r - 1; }
    std::uint32_t row_slot(std::uint32_t c) const noexcept {
        return shape_.grid - 1 + (c < bj_ ? c : c - 1);
    }
    std::uint32_t slot_of(std::uint32_t from) const noexcept {
        if (from >= shape_.blocks) return kNoSlot;
        const std::uint32_t r = from / shape_.grid;
        const std::uint32_t c = from % shape_.grid;
        if (c == bj_ && r != bi_) return col_slot(r);
        if (r == bi_ && c != bj_) return row_slot(c);
        return kNoSlot;
    }

    Shape                shape_{};
    std::uint32_t        id_{0};
    std::uint32_t        bi_{0};
    std::uint32_t        bj_{0};
    State               *state_{nullptr};
    const std::int64_t **inbox_{nullptr};
    std::int64_t        *tile_[2]{nullptr, nullptr};
};

// ---------------------------------------------------------------------------------------------
// The verified answer
// ---------------------------------------------------------------------------------------------

// One block's tally in the checksum: its id, the generations it computed, the tiles it accepted
// and the witness of its phases, mixed -- a block that skipped a phase, ran one twice, computed one
// wrong, or accepted a tile too many or too few moves the total, even when its final tile is
// right (witness() says why that is not enough).
inline std::uint64_t tally_digest(std::uint32_t id, std::uint64_t phases, std::uint64_t received,
                                  std::uint64_t work) noexcept {
    return qvo::mix(qvo::mix(qvo::mix((static_cast<std::uint64_t>(id) << 40) ^ phases) ^ received)
                    ^ work);
}

// One block's tile in the checksum: every distance weighted by its place in the matrix, so a tile
// reported for another block, or one distance wrong, moves the total. `origin` is the tile's
// (0, 0) and `stride` its row pitch: `block` for a block's tile, `nodes` for the reference matrix.
inline constexpr std::uint64_t kCellSalt = 0xa5b5c5d5e5f50000ULL;
inline std::uint64_t tile_digest(const Shape &s, std::uint32_t id, const std::int64_t *origin,
                                 std::size_t stride) noexcept {
    const std::uint64_t r0  = static_cast<std::uint64_t>(id / s.grid) * s.block;
    const std::uint64_t c0  = static_cast<std::uint64_t>(id % s.grid) * s.block;
    std::uint64_t       acc = 0;
    for (std::uint32_t i = 0; i < s.block; ++i)
        for (std::uint32_t j = 0; j < s.block; ++j)
            acc += qvo::mix(qvo::mix(kCellSalt + (r0 + i) * s.nodes + c0 + j)
                            + static_cast<std::uint64_t>(origin[i * stride + j]));
    return acc;
}

// What every adapter returns, computed after teardown from the Storage: the blocks' tallies and
// tiles (each block's latest generation), and every delivery the handlers ran -- the tiles and
// Initials the blocks accepted, plus the Dones the sink counted.
inline std::uint64_t checksum(const Storage &st) noexcept {
    const Shape  &s   = st.shape();
    std::uint64_t acc = 0;
    for (std::uint32_t b = 0; b < s.blocks; ++b) {
        const State &x = st.state(b);
        acc += tally_digest(b, x.generation, x.received, x.work)
               + tile_digest(s, b, st.tile(b, x.generation & 1u), s.block);
    }
    return acc;
}
inline std::uint64_t delivered(const Storage &st, std::uint64_t dones) noexcept {
    std::uint64_t n = dones;
    for (std::uint32_t b = 0; b < st.shape().blocks; ++b) n += st.state(b).received + st.state(b).initial;
    return n;
}

// The observation (qvo::Answer::observed): tiles that reached a block one generation AHEAD of its
// own -- a neighbour already done with the phase this block is still collecting. How far the
// blocks drift apart within the one generation the protocol allows depends on the interleaving:
// reported beside the cell, never asserted (FAIRNESS.md section 0).
inline constexpr const char *kObservedAhead = "ahead";
inline std::uint64_t ahead(const Storage &st) noexcept {
    std::uint64_t n = 0;
    for (std::uint32_t b = 0; b < st.shape().blocks; ++b) n += st.state(b).ahead;
    return n;
}

// The reference: Floyd-Warshall IN PLACE over the whole matrix -- the textbook loop, sharing no
// code with the blocks' phases (a kernel that mis-indexed a pivot would disagree with it, not agree
// by construction). In place, the matrix after pivot k IS generation k + 1 of the phases: during
// pivot k row k and column k do not change (D[k][k] = 0, the weights are positive), so every entry
// reads the generation-k values the blocks read. That is what lets the reference compute each
// block's witness -- the sum of its tile after every pivot -- as well as the final matrix. The
// result is checked against what any shortest-path matrix of an undirected graph is -- zero on the
// diagonal, symmetric, never above the edge -- so the reference cannot silently be wrong.
// O(nodes^3), outside the window: called once, before the first repetition.
struct Reference {
    std::vector<std::int64_t>  matrix;
    std::vector<std::uint64_t> witness;  // per block
};
inline Reference reference(const Shape &s) {
    Reference         r{graph(s.nodes, s.weight), std::vector<std::uint64_t>(s.blocks, 0)};
    auto             &d = r.matrix;
    const std::size_t n = s.nodes;
    std::vector<std::uint64_t> sums(s.blocks);
    for (std::size_t k = 0; k < n; ++k) {
        for (auto &x : sums) x = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::int64_t dik = d[i * n + k];
            std::uint64_t     *row = sums.data() + (i / s.block) * s.grid;
            for (std::size_t c = 0; c < s.grid; ++c) {
                std::uint64_t acc = 0;
                for (std::size_t j = c * s.block; j < (c + 1) * s.block; ++j) {
                    if (dik + d[k * n + j] < d[i * n + j]) d[i * n + j] = dik + d[k * n + j];
                    acc += static_cast<std::uint64_t>(d[i * n + j]);
                }
                row[c] += acc;
            }
        }
        for (std::uint32_t b = 0; b < s.blocks; ++b) r.witness[b] = witness(r.witness[b], sums[b]);
    }
    const std::vector<std::int64_t> g = graph(s.nodes, s.weight);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            if ((i == j && d[i * n + j] != 0) || d[i * n + j] != d[j * n + i]
                || d[i * n + j] > g[i * n + j] || (i != j && d[i * n + j] < 1))
                qvo::refuse("savina/apsp: the reference's matrix is not a shortest-path matrix at ("
                            + std::to_string(i) + ", " + std::to_string(j) + ")");
    return r;
}

// The framework-free answer: every block having computed `nodes` generations from
// `neighbours * nodes` accepted tiles, with the reference's witness, and holding its part of the
// reference matrix.
inline std::uint64_t expected(const qvo::Params &p) {
    const Shape     s   = shape(p);
    const Reference r   = reference(s);
    std::uint64_t   acc = 0;
    for (std::uint32_t b = 0; b < s.blocks; ++b) {
        const std::size_t r0 = static_cast<std::size_t>(b / s.grid) * s.block;
        const std::size_t c0 = static_cast<std::size_t>(b % s.grid) * s.block;
        acc += tally_digest(b, s.nodes, static_cast<std::uint64_t>(s.neighbours) * s.nodes,
                            r.witness[b])
               + tile_digest(s, b, r.matrix.data() + r0 * s.nodes + c0, s.nodes);
    }
    return acc;
}

// The report divides by this: one phase of one block -- block^2 min-plus steps and the tile sent
// to every neighbour.
inline constexpr const char *kWorkUnit = "block phase";
inline std::uint64_t work_units(const qvo::Params &p) {
    const Shape s = shape(p);
    return static_cast<std::uint64_t>(s.blocks) * s.nodes;
}

// COUNTED at the receivers, inside the window: every block accepts one Initial and
// `neighbours` tiles of each generation 0 .. nodes-1, and the sink one Done per block. The
// handshake before the window and the shutdown after it are not counted.
inline std::uint64_t expected_messages(const qvo::Params &p) {
    const Shape s = shape(p);
    return static_cast<std::uint64_t>(s.blocks) * (static_cast<std::uint64_t>(s.neighbours) * s.nodes + 2);
}


#ifdef QVO_REAL_ADAPTER_CONTROLS
// Compiled only into qvoctl-* built from the real adapters. No plant code reaches a table binary.
// Each block owns its plant state; only block 7 (block 0 for early) can fire.
class AdapterControl {
public:
    AdapterControl(Storage &storage, std::uint32_t id)
        : storage_(storage), id_(id), mode_(control_mode()) {}

    bool accept(Block &b, std::uint32_t g, std::uint32_t from, const std::int64_t *tile) {
        const Shape &s = storage_.shape();
        const bool target = id_ == 7 % s.blocks && g == s.nodes / 2 && !fired_;
        if (target && mode_ == "lost") {
            mark();
            return false;
        }
        if (target && mode_ == "misroute") {
            mark();
            // Corrupt the dispatch origin to a diagonal block: never a row/column neighbour.
            from = ((id_ / s.grid + 1) % s.grid) * s.grid + (id_ % s.grid + 1) % s.grid;
        }
        if (target && mode_ == "far-ahead") {
            mark();
            g = b.generation() + 2;
        }
        if (target && mode_ == "misroute-row") {
            // File a received tile in another, already occupied row-neighbour slot.
            const auto parity = g & 1u;
            for (std::uint32_t c = 0; c < s.grid; ++c) {
                if (c == id_ % s.grid) continue;
                const auto slot = s.grid - 1 + (c < id_ % s.grid ? c : c - 1);
                if (storage_.inbox(id_)[parity * s.neighbours + slot] != nullptr) {
                    mark();
                    from = (id_ / s.grid) * s.grid + c;
                    break;
                }
            }
        }
        bool complete = b.accept(g, from, tile);
        if (target && mode_ == "duplicate") {
            mark();
            b.accept(g, from, tile);  // before compute clears this generation's inbox
        }
        if (mode_ == "early" && !fired_ && id_ == 0 && b.generation() == 3
            && storage_.state(id_).pending[1] == s.neighbours - 1) {
            mark();
            complete = true;  // the real compute() call must reject the incomplete phase
        }
        return complete;
    }

    const std::int64_t *compute(Block &b) {
        const auto own = b.generation();
        const auto *tile = b.compute();
        const Shape &s = storage_.shape();
        if (mode_ == "skip-change" && !fired_ && id_ == 7 % s.blocks) {
            auto *next = storage_.tile(id_, (own + 1) & 1u);
            const auto *prev = storage_.tile(id_, own & 1u);
            const std::size_t n = static_cast<std::size_t>(s.block) * s.block;
            bool changed = false;
            for (std::size_t i = 0; i < n; ++i) changed = changed || next[i] != prev[i];
            if (changed) {
                mark();
                // Discard only actual adapter storage, after the phase's witness was computed.
                for (std::size_t i = 0; i < n; ++i) next[i] = prev[i];
            }
        }
        return tile;
    }

private:
    static std::string control_mode() {
        const char *v = std::getenv("QVO_CONTROL_PLANT");
        const std::string mode = v ? v : "none";
        if (mode != "none" && mode != "duplicate" && mode != "lost" && mode != "misroute"
            && mode != "misroute-row" && mode != "far-ahead" && mode != "early"
            && mode != "skip-change")
            qvo::refuse("savina/apsp adapter control: unknown plant " + mode);
        return mode;
    }
    void mark() {
        fired_ = true;
        std::fprintf(stderr, "control plant fired: %s\n", mode_.c_str());
        std::fflush(stderr);
    }
    Storage &storage_;
    std::uint32_t id_;
    std::string mode_;
    bool fired_{false};
};

inline void explain_adapter_control(const Storage &storage) {
    const Shape &s = storage.shape();
    const Reference ref = reference(s);  // unmodified independent reference, after teardown
    bool counts = true, matrix = true;
    for (std::uint32_t b = 0; b < s.blocks; ++b) {
        const State &x = storage.state(b);
        const auto *tile = storage.tile(b, x.generation & 1u);
        counts = counts && x.generation == s.nodes
                 && x.received == static_cast<std::uint64_t>(s.neighbours) * s.nodes;
        const std::size_t r0 = static_cast<std::size_t>(b / s.grid) * s.block;
        const std::size_t c0 = static_cast<std::size_t>(b % s.grid) * s.block;
        for (std::size_t i = 0; i < s.block; ++i)
            for (std::size_t j = 0; j < s.block; ++j)
                matrix = matrix && tile[i * s.block + j] == ref.matrix[(r0 + i) * s.nodes + c0 + j];
    }
    std::fprintf(stderr, "apsp adapter control: every block's phases and tiles as the closed form says: %s\n",
                 counts ? "yes" : "no");
    std::fprintf(stderr, "apsp adapter control: the final matrix as the reference says: %s\n",
                 matrix ? "yes" : "no");
}
#endif

}  // namespace qvospec::savina::apsp

#endif  // QVOSPEC_SAVINA_APSP_H
