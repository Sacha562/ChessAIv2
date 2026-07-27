#include "corrhist.hpp"

#include "chess.hpp"

#include <algorithm>
#include <cstring>

using namespace chess;

namespace engine {

namespace {

// Multiplicative mixing constants (odd, high entropy) for the pawn-structure hash: the
// golden-ratio and a second well-distributed 64-bit odd constant.
constexpr uint64_t MIX_WHITE = 0x9E3779B97F4A7C15ULL;
constexpr uint64_t MIX_BLACK = 0xC2B2AE3D27D4EB4FULL;

} // namespace

uint64_t pawnStructureKey(const Board& board) {
    const uint64_t white = board.pieces(PieceType::PAWN, Color::WHITE).getBits();
    const uint64_t black = board.pieces(PieceType::PAWN, Color::BLACK).getBits();
    uint64_t       key   = white * MIX_WHITE ^ black * MIX_BLACK;
    key ^= key >> 29; // avalanche the high bits down into the index bits
    return key * MIX_BLACK;
}

void CorrectionHistory::clear() {
    std::memset(table_, 0, sizeof(table_));
}

int CorrectionHistory::index(const Board& board) {
    return static_cast<int>((pawnStructureKey(board) >> 32) & (SIZE - 1));
}

Value CorrectionHistory::correction(const Board& board) const {
    return static_cast<Value>(table_[board.sideToMove()][index(board)] / CORR_GRAIN);
}

Value CorrectionHistory::corrected(const Board& board, Value raw) const {
    const Value adjusted = raw + correction(board);
    return std::clamp<Value>(adjusted, -VALUE_MATE_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}

void CorrectionHistory::update(const Board& board, int depth, Value diff) {
    int16_t&  entry   = table_[board.sideToMove()][index(board)];
    const int weight  = std::min(depth + 1, MAX_WEIGHT);
    const int scaled  = std::clamp(diff * CORR_GRAIN, -CORR_LIMIT, CORR_LIMIT);
    const int updated = (entry * (WEIGHT_DIV - weight) + scaled * weight) / WEIGHT_DIV;
    entry             = static_cast<int16_t>(std::clamp(updated, -CORR_LIMIT, CORR_LIMIT));
}

} // namespace engine
