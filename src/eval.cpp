#include "eval.hpp"
#include "chess.hpp"

using namespace chess;

namespace engine {

namespace {

// --- PeSTO / Kaufman seeds ------------------------------------------------------
// Material base per piece type (P,N,B,R,Q,K), midgame and endgame. Folded into the
// piece-square tables below to form the combined DEFAULT_EVAL_PARAMS. King has no
// material value (its safety/activity lives entirely in its PSQT).
constexpr std::array<int, 6> MG_VALUE = {82, 337, 365, 477, 1025, 0};
constexpr std::array<int, 6> EG_VALUE = {94, 281, 297, 512, 936, 0};

// Positional deltas, White's a8-first orientation (row 0 = rank 8). These are the
// canonical PeSTO tables (Rofchade, via the Chess Programming Wiki); they are only
// seeds — Texel tuning re-fits every entry.
constexpr std::array<int16_t, 64> MG_PAWN = {
    0,   0,  0,   0,   0,   0,  0,  0,   98,  134, 61, 95,  68, 126, 34, -11,
    -6,  7,  26,  31,  65,  56, 25, -20, -14, 13,  6,  21,  23, 12,  17, -23,
    -27, -2, -5,  12,  17,  6,  10, -25, -26, -4,  -4, -10, 3,  3,   33, -12,
    -35, -1, -20, -23, -15, 24, 38, -22, 0,   0,   0,  0,   0,  0,   0,  0};
constexpr std::array<int16_t, 64> EG_PAWN = {
    0,  0,  0,  0,  0,  0, 0,  0,  178, 173, 158, 134, 147, 132, 165, 187, 94, 100, 85, 67, 56, 53,
    82, 84, 32, 24, 13, 5, -2, 4,  17,  17,  13,  9,   -3,  -7,  -7,  -8,  3,  -1,  4,  7,  -6, 1,
    0,  -5, -1, -8, 13, 8, 8,  10, 13,  0,   2,   -7,  0,   0,   0,   0,   0,  0,   0,  0};
constexpr std::array<int16_t, 64> MG_KNIGHT = {
    -167, -89, -34, -49, 61, -97, -15, -107, -73,  -41, 72,  36,  23,  62,  7,   -17,
    -47,  60,  37,  65,  84, 129, 73,  44,   -9,   17,  19,  53,  37,  69,  18,  22,
    -13,  4,   16,  13,  28, 19,  21,  -8,   -23,  -9,  12,  10,  19,  17,  25,  -16,
    -29,  -53, -12, -3,  -1, 18,  -14, -19,  -105, -21, -58, -33, -17, -28, -19, -23};
constexpr std::array<int16_t, 64> EG_KNIGHT = {
    -58, -38, -13, -28, -31, -27, -63, -99, -25, -8,  -25, -2,  -9,  -25, -24, -52,
    -24, -20, 10,  9,   -1,  -9,  -19, -41, -17, 3,   22,  22,  22,  11,  8,   -18,
    -18, -6,  16,  25,  16,  17,  4,   -18, -23, -3,  -1,  15,  10,  -3,  -20, -22,
    -42, -20, -10, -5,  -2,  -20, -23, -44, -29, -51, -23, -15, -22, -18, -50, -64};
constexpr std::array<int16_t, 64> MG_BISHOP = {
    -29, 4,  -82, -37, -25, -42, 7,  -8, -26, 16, -18, -13, 30,  59,  18,  -47,
    -16, 37, 43,  40,  35,  50,  37, -2, -4,  5,  19,  50,  37,  37,  7,   -2,
    -6,  13, 13,  26,  34,  12,  10, 4,  0,   15, 15,  15,  14,  27,  18,  10,
    4,   15, 16,  0,   7,   21,  33, 1,  -33, -3, -14, -21, -13, -12, -39, -21};
constexpr std::array<int16_t, 64> EG_BISHOP = {
    -14, -21, -11, -8, -7, -9, -17, -24, -8,  -4, 7,   -12, -3, -13, -4, -14,
    2,   -8,  0,   -1, -2, 6,  0,   4,   -3,  9,  12,  9,   14, 10,  3,  2,
    -6,  3,   13,  19, 7,  10, -3,  -9,  -12, -3, 8,   10,  13, 3,   -7, -15,
    -14, -18, -7,  -1, 4,  -9, -15, -27, -23, -9, -23, -5,  -9, -16, -5, -17};
constexpr std::array<int16_t, 64> MG_ROOK = {
    32,  42,  32,  51, 63, 9,  31, 43,  27,  32,  58,  62,  80, 67, 26,  44,
    -5,  19,  26,  36, 17, 45, 61, 16,  -24, -11, 7,   26,  24, 35, -8,  -20,
    -36, -26, -12, -1, 9,  -7, 6,  -23, -45, -25, -16, -17, 3,  0,  -5,  -33,
    -44, -16, -20, -9, -1, 11, -6, -71, -19, -13, 1,   17,  16, 7,  -37, -26};
constexpr std::array<int16_t, 64> EG_ROOK = {
    13, 10,  18, 15,  12, 12, 8, 5, 11, 13, 13,  11, -3, 3, 8,  3,  7,  7,   7,  5,  4,  -3,
    -5, -3,  4,  3,   13, 1,  2, 1, -1, 2,  3,   5,  8,  4, -5, -6, -8, -11, -4, 0,  -5, -1,
    -7, -12, -8, -16, -6, -6, 0, 2, -9, -9, -11, -3, -9, 2, 3,  -1, -5, -13, 4,  -20};
constexpr std::array<int16_t, 64> MG_QUEEN = {
    -28, 0,   29, 12,  59, 44, 43, 45, -24, -39, -5,  1,   -16, 57,  28,  54,
    -13, -17, 7,  8,   29, 56, 47, 57, -27, -27, -16, -16, -1,  17,  -2,  1,
    -9,  -26, -9, -10, -2, -4, 3,  -3, -14, 2,   -11, -2,  -5,  2,   14,  5,
    -35, -8,  11, 2,   8,  15, -3, 1,  -1,  -18, -9,  10,  -15, -25, -31, -50};
constexpr std::array<int16_t, 64> EG_QUEEN = {
    -9,  22,  22,  27,  27,  19,  10,  20,  -17, 20,  32,  41,  58, 25,  30,  0,
    -20, 6,   9,   49,  47,  35,  19,  9,   3,   22,  24,  45,  57, 40,  57,  36,
    -18, 28,  19,  47,  31,  34,  39,  23,  -16, -27, 15,  6,   9,  17,  10,  5,
    -22, -23, -30, -16, -16, -23, -36, -32, -33, -28, -22, -43, -5, -32, -20, -41};
constexpr std::array<int16_t, 64> MG_KING = {
    -65, 23, 16,  -15, -56, -34, 2,   13,  29,  -1,  -20, -7,  -8,  -4,  -38, -29,
    -9,  24, 2,   -16, -20, 6,   22,  -22, -17, -20, -12, -27, -30, -25, -14, -36,
    -49, -1, -27, -39, -46, -44, -33, -51, -14, -14, -22, -46, -44, -30, -15, -27,
    1,   7,  -8,  -64, -43, -16, 9,   8,   -15, 36,  12,  -54, 8,   -28, 24,  14};
constexpr std::array<int16_t, 64> EG_KING = {
    -74, -35, -18, -18, -11, 15, 4,  -17, -12, 17,  14,  17,  17,  38,  23,  11,
    10,  17,  23,  15,  20,  45, 44, 13,  -8,  22,  24,  27,  26,  33,  26,  3,
    -18, -4,  21,  24,  27,  23, 9,  -11, -19, -3,  11,  21,  23,  16,  7,   -9,
    -27, -11, 4,   13,  14,  4,  -5, -17, -53, -34, -21, -11, -28, -14, -24, -43};

constexpr std::array<std::array<int16_t, 64>, 6> MG_DELTA = {MG_PAWN, MG_KNIGHT, MG_BISHOP,
                                                             MG_ROOK, MG_QUEEN,  MG_KING};
constexpr std::array<std::array<int16_t, 64>, 6> EG_DELTA = {EG_PAWN, EG_KNIGHT, EG_BISHOP,
                                                             EG_ROOK, EG_QUEEN,  EG_KING};

// Fold material into the positional deltas to form the combined default tables.
constexpr EvalParams buildDefaultParams() {
    EvalParams p{};
    for (int pt = 0; pt < 6; ++pt) {
        for (int sq = 0; sq < 64; ++sq) {
            p.mg[pt][sq] = static_cast<int16_t>(MG_VALUE[pt] + MG_DELTA[pt][sq]);
            p.eg[pt][sq] = static_cast<int16_t>(EG_VALUE[pt] + EG_DELTA[pt][sq]);
        }
    }
    return p;
}

// Piece types in EvalParams index order (0..5).
constexpr std::array<PieceType, 6> PIECE_TYPES = {PieceType::PAWN,   PieceType::KNIGHT,
                                                  PieceType::BISHOP, PieceType::ROOK,
                                                  PieceType::QUEEN,  PieceType::KING};

// Tapered-eval phase weight per piece (both sides summed); non-pawn material only.
constexpr std::array<int, 6> PHASE_WEIGHT = {0, 1, 1, 2, 4, 0};
constexpr int                PHASE_MAX    = 24; // 2 * (2*1 + 2*1 + 2*2 + 1*4)

} // namespace

const EvalParams DEFAULT_EVAL_PARAMS = buildDefaultParams();

Value evaluate(const Board& board, const EvalParams& params, Value tempo) {
    int mg = 0, eg = 0, phase = 0;

    for (int pt = 0; pt < 6; ++pt) {
        const PieceType type  = PIECE_TYPES[pt];
        Bitboard        white = board.pieces(type, Color::WHITE);
        Bitboard        black = board.pieces(type, Color::BLACK);

        phase += PHASE_WEIGHT[pt] * (white.count() + black.count());

        while (white) {
            const int sq = white.pop() ^ 56; // White reads the a8-first table flipped
            mg += params.mg[pt][sq];
            eg += params.eg[pt][sq];
        }
        while (black) {
            const int sq = static_cast<int>(black.pop()); // Black mirrors it (reads directly)
            mg -= params.mg[pt][sq];
            eg -= params.eg[pt][sq];
        }
    }

    // Early promotions can push the phase above the cap; clamp so the blend stays in
    // [eg, mg].
    if (phase > PHASE_MAX) phase = PHASE_MAX;
    const int tapered = (mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX;

    // Flip to the side-to-move's perspective (negamax), then add the tempo bonus.
    const Value stm = (board.sideToMove() == Color::WHITE) ? tapered : -tapered;
    return stm + tempo;
}

Value evaluate(const Board& board, Value tempo) {
    return evaluate(board, DEFAULT_EVAL_PARAMS, tempo);
}

} // namespace engine
