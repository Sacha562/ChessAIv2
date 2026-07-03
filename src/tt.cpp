#include "tt.hpp"

#include <algorithm>

#if defined(__GNUC__) || defined(__clang__)
#include <xmmintrin.h>
#endif

namespace engine {

namespace {

// data layout (LSB -> MSB): move[16] value[16] eval[16] depth[8] genBound[8].
constexpr int VALUE_SHIFT = 16;
constexpr int EVAL_SHIFT  = 32;
constexpr int DEPTH_SHIFT = 48;
constexpr int GEN_SHIFT   = 56;

uint64_t packData(uint16_t move, Value value, Value eval, int depth, uint8_t genBound) {
    return static_cast<uint64_t>(move)
         | (static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(value))) << VALUE_SHIFT)
         | (static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(eval))) << EVAL_SHIFT)
         | (static_cast<uint64_t>(static_cast<uint8_t>(static_cast<int8_t>(depth))) << DEPTH_SHIFT)
         | (static_cast<uint64_t>(genBound) << GEN_SHIFT);
}

uint16_t unpackMove(uint64_t data)  { return static_cast<uint16_t>(data); }
Value    unpackValue(uint64_t data) { return static_cast<int16_t>(data >> VALUE_SHIFT); }
Value    unpackEval(uint64_t data)  { return static_cast<int16_t>(data >> EVAL_SHIFT); }
int      unpackDepth(uint64_t data) { return static_cast<int8_t>(data >> DEPTH_SHIFT); }
uint8_t  unpackGenBound(uint64_t data) { return static_cast<uint8_t>(data >> GEN_SHIFT); }

Bound   boundOf(uint8_t genBound) { return static_cast<Bound>(genBound & 0x3); }
uint8_t genOf(uint8_t genBound)   { return static_cast<uint8_t>(genBound >> 2); }

} // namespace

Value valueToTT(Value v, int ply) {
    if (v >= VALUE_MATE_IN_MAX_PLY) return v + ply;
    if (v <= VALUE_MATED_IN_MAX_PLY) return v - ply;
    return v;
}

Value valueFromTT(Value v, int ply) {
    if (v == VALUE_NONE) return VALUE_NONE;
    if (v >= VALUE_MATE_IN_MAX_PLY) return v - ply;
    if (v <= VALUE_MATED_IN_MAX_PLY) return v + ply;
    return v;
}

void TranspositionTable::resize(size_t mb) {
    size_t clusters = (mb * 1024 * 1024) / sizeof(Cluster);
    if (clusters < 1) clusters = 1;
    table_.assign(clusters, Cluster{});
    generation_ = 0;
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), Cluster{});
    generation_ = 0;
}

size_t TranspositionTable::clusterIndex(uint64_t key) const {
    // Multiply-shift: map the full key uniformly onto [0, table_.size()).
    return static_cast<size_t>(
        (static_cast<__uint128_t>(key) * static_cast<__uint128_t>(table_.size())) >> 64);
}

int TranspositionTable::relativeAge(uint8_t genBound) const {
    return (GEN_CYCLE + generation_ - genOf(genBound)) & GEN_MASK;
}

void TranspositionTable::prefetch(uint64_t key) const {
#if defined(__GNUC__) || defined(__clang__)
    _mm_prefetch(reinterpret_cast<const char*>(&table_[clusterIndex(key)]), _MM_HINT_T0);
#else
    static_cast<void>(key);
#endif
}

TTProbe TranspositionTable::probe(uint64_t key) const {
    const Cluster& cluster = table_[clusterIndex(key)];
    for (const Entry& e : cluster.entries) {
        if (e.data != 0 && (e.key ^ e.data) == key) {
            const uint8_t genBound = unpackGenBound(e.data);
            return TTProbe{true, Move(unpackMove(e.data)), unpackValue(e.data),
                           unpackEval(e.data), unpackDepth(e.data), boundOf(genBound)};
        }
    }
    return TTProbe{};
}

void TranspositionTable::store(uint64_t key, int depth, Value value, Value eval, Move move,
                               Bound bound) {
    Cluster& cluster = table_[clusterIndex(key)];

    Entry* replace = &cluster.entries[0];
    for (Entry& e : cluster.entries) {
        if (e.data == 0 || (e.key ^ e.data) == key) {   // empty slot, or same position
            replace = &e;
            break;
        }
        // Otherwise keep the entry worth the most (deep + recent); replace the rest.
        const int eWorth = unpackDepth(e.data) - relativeAge(unpackGenBound(e.data)) * 8;
        const int rWorth = unpackDepth(replace->data)
                         - relativeAge(unpackGenBound(replace->data)) * 8;
        if (eWorth < rWorth) replace = &e;
    }

    // Preserve the stored best move when this store carries none for the same slot.
    const bool sameSlot = replace->data != 0 && (replace->key ^ replace->data) == key;
    uint16_t moveBits = move.move();
    if (moveBits == Move(Move::NO_MOVE).move() && sameSlot)
        moveBits = unpackMove(replace->data);

    const uint8_t genBound = static_cast<uint8_t>((generation_ << 2) | bound);
    const uint64_t data = packData(moveBits, value, eval, depth, genBound);
    replace->key  = key ^ data;
    replace->data = data;
}

int TranspositionTable::hashfull() const {
    int used = 0;
    int slots = 0;
    for (const Cluster& cluster : table_) {
        for (const Entry& e : cluster.entries) {
            if (slots >= 1000) return used;
            ++slots;
            if (e.data != 0 && relativeAge(unpackGenBound(e.data)) == 0) ++used;
        }
    }
    return slots > 0 ? used * 1000 / slots : 0;
}

} // namespace engine
