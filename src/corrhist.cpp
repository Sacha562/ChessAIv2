#include "corrhist.hpp"

#include "chess.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

using namespace chess;

namespace engine {

namespace {

constexpr int PERCENT = 100;

// Multiplicative mixing constants (odd, high entropy), one per (colour, piece type), so
// the same bitboard of two different piece kinds hashes differently. The pawn pair and
// FINAL_MIX are the original pawn-key constants, which keeps that key unchanged.
constexpr uint64_t MIX[2][6] = {
    {0x9E3779B97F4A7C15ULL, 0xFF51AFD7ED558CCDULL, 0xC4CEB9FE1A85EC53ULL, 0xBF58476D1CE4E5B9ULL,
     0x94D049BB133111EBULL, 0x2545F4914F6CDD1DULL},
    {0xC2B2AE3D27D4EB4FULL, 0x87C37B91114253D5ULL, 0x4CF5AD432745937FULL, 0x9E3779B185EBCA87ULL,
     0x165667B19E3779F9ULL, 0x85EBCA77C2B2AE63ULL},
};
constexpr uint64_t FINAL_MIX = 0xC2B2AE3D27D4EB4FULL;

// The piece sets each key covers.
constexpr PieceType PAWNS[]     = {PieceType::PAWN};
constexpr PieceType NON_PAWNS[] = {PieceType::KNIGHT, PieceType::BISHOP, PieceType::ROOK,
                                   PieceType::QUEEN, PieceType::KING};
constexpr PieceType MINORS[]    = {PieceType::KNIGHT, PieceType::BISHOP, PieceType::KING};
constexpr PieceType MAJORS[]    = {PieceType::ROOK, PieceType::QUEEN, PieceType::KING};

// Mix of `color`'s pieces of every type in `types` (not yet avalanched).
template <size_t N> uint64_t sideMix(const Board& board, Color color, const PieceType (&types)[N]) {
    const int c   = static_cast<int>(color);
    uint64_t  key = 0;
    for (const PieceType type : types)
        key ^= board.pieces(type, color).getBits() * MIX[c][static_cast<int>(type)];
    return key;
}

// Avalanche the high bits down into the index bits.
uint64_t finish(uint64_t key) {
    key ^= key >> 29;
    return key * FINAL_MIX;
}

// The key over `types` for both colours.
template <size_t N> uint64_t bothSidesKey(const Board& board, const PieceType (&types)[N]) {
    return finish(sideMix(board, Color::WHITE, types) ^ sideMix(board, Color::BLACK, types));
}

// Gravity update of one entry: move it by `bonus`, damped as it nears the limit, so a
// saturated entry still yields to residuals of the other sign.
void gravity(int16_t& entry, int bonus) {
    const int updated = entry + bonus - entry * std::abs(bonus) / CORR_LIMIT;
    entry             = static_cast<int16_t>(std::clamp(updated, -CORR_LIMIT, CORR_LIMIT));
}

} // namespace

uint64_t pawnStructureKey(const Board& board) {
    return bothSidesKey(board, PAWNS);
}

uint64_t nonPawnKey(const Board& board, Color color) {
    return finish(sideMix(board, color, NON_PAWNS));
}

uint64_t minorKey(const Board& board) {
    return bothSidesKey(board, MINORS);
}

uint64_t majorKey(const Board& board) {
    return bothSidesKey(board, MAJORS);
}

CorrectionHistory::CorrectionHistory() : tables_(std::make_unique<Tables>()) {
    clear();
}

void CorrectionHistory::clear() {
    std::memset(tables_.get(), 0, sizeof(Tables));
}

int CorrectionHistory::bucket(uint64_t key) {
    return static_cast<int>((key >> 32) & (SIZE - 1));
}

CorrSlots CorrectionHistory::slots(const Board& board, int prevPiece, int prevTo) {
    CorrSlots s;
    s.stm        = static_cast<int>(board.sideToMove());
    s.pawn       = bucket(pawnStructureKey(board));
    s.nonPawn[0] = bucket(nonPawnKey(board, Color::WHITE));
    s.nonPawn[1] = bucket(nonPawnKey(board, Color::BLACK));
    s.minor      = bucket(minorKey(board));
    s.major      = bucket(majorKey(board));
    s.contPiece  = prevPiece;
    s.contTo     = prevTo;
    return s;
}

Value CorrectionHistory::correction(const CorrSlots& s) const {
    const Tables& t = *tables_;
    int           sum =
        weights_.pawn * t.pawn[s.stm][s.pawn] +
        weights_.nonPawn * (t.nonPawn[s.stm][0][s.nonPawn[0]] + t.nonPawn[s.stm][1][s.nonPawn[1]]) +
        weights_.minor * t.minor[s.stm][s.minor] + weights_.major * t.major[s.stm][s.major];
    if (s.contPiece >= 0) sum += weights_.cont * t.cont[s.stm][s.contPiece][s.contTo];
    return static_cast<Value>(sum / (PERCENT * CORR_GRAIN));
}

Value CorrectionHistory::corrected(const CorrSlots& s, Value raw) const {
    const Value adjusted = raw + correction(s);
    return std::clamp<Value>(adjusted, -VALUE_MATE_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}

void CorrectionHistory::update(const CorrSlots& s, int depth, Value diff) {
    const int weight = std::min(depth + 1, MAX_WEIGHT);
    const int bonus  = std::clamp(diff * CORR_GRAIN * weight / BONUS_DIV, -MAX_BONUS, MAX_BONUS);
    Tables&   t      = *tables_;
    gravity(t.pawn[s.stm][s.pawn], bonus);
    gravity(t.nonPawn[s.stm][0][s.nonPawn[0]], bonus);
    gravity(t.nonPawn[s.stm][1][s.nonPawn[1]], bonus);
    gravity(t.minor[s.stm][s.minor], bonus);
    gravity(t.major[s.stm][s.major], bonus);
    if (s.contPiece >= 0) gravity(t.cont[s.stm][s.contPiece][s.contTo], bonus);
}

} // namespace engine
