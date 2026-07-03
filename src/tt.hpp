#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "types.hpp"

namespace engine {

// How a stored value relates to the true score of a position.
enum Bound : uint8_t {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,   // fail-low:  value is an upper bound (true score <= value)
    BOUND_LOWER = 2,   // fail-high: value is a lower bound (true score >= value)
    BOUND_EXACT = 3,   // PV node:   value is exact
};

// Result of a TT probe. `value`/`eval` are the raw stored numbers; a mate `value`
// is relative to the stored node and must be rebased with `valueFromTT` before use.
struct TTProbe {
    bool  hit   = false;
    Move  move  = Move(Move::NO_MOVE);
    Value value = VALUE_NONE;
    Value eval  = VALUE_NONE;
    int   depth = 0;
    Bound bound = BOUND_NONE;
};

// Mate scores are stored relative to the node, not the root, so an entry stays
// valid at any depth it transposes to. Convert on the way in/out of the table.
Value valueToTT(Value v, int ply);
Value valueFromTT(Value v, int ply);

// A bucketed, aged, lock-less transposition table. Positions are keyed by the
// board's 64-bit Zobrist hash. Concurrency uses the Hyatt XOR trick (store
// `key ^ data`; a reader recovers and validates the key, so a torn write from
// another thread self-detects as a miss) — ready for Lazy SMP in Phase 1d.
class TranspositionTable {
public:
    explicit TranspositionTable(size_t mb = 16) { resize(mb); }

    // (Re)allocate to `mb` megabytes (at least one cluster) and clear.
    void resize(size_t mb);
    // Zero every entry and reset the generation counter.
    void clear();
    // Advance the generation so this search's stores age out older entries.
    void newSearch() { generation_ = static_cast<uint8_t>((generation_ + 1) & GEN_MASK); }

    TTProbe probe(uint64_t key) const;
    void store(uint64_t key, int depth, Value value, Value eval, Move move, Bound bound);

    // Hint the CPU to pull a key's cluster into cache before it is probed.
    void prefetch(uint64_t key) const;
    // Fill estimate in permille (current-generation entries in a 1000-slot sample).
    int hashfull() const;

private:
    // Hyatt lock-less slot: `key` holds `zobrist ^ data`; `data` packs the payload.
    struct Entry {
        uint64_t key  = 0;
        uint64_t data = 0;
    };

    static constexpr size_t CLUSTER_SIZE = 4;   // 4 x 16B = one 64-byte cache line
    struct alignas(64) Cluster {
        Entry entries[CLUSTER_SIZE];
    };

    static constexpr uint8_t GEN_BITS  = 6;
    static constexpr uint8_t GEN_MASK  = (1U << GEN_BITS) - 1;   // 63
    static constexpr uint8_t GEN_CYCLE = 1U << GEN_BITS;         // 64

    size_t clusterIndex(uint64_t key) const;
    int    relativeAge(uint8_t genBound) const;

    std::vector<Cluster> table_;
    uint8_t generation_ = 0;
};

} // namespace engine
