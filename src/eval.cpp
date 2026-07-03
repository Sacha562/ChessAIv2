#include "eval.hpp"
#include "chess.hpp"

using namespace chess;

namespace engine {

// Phase 0 evaluation: pure material balance plus a small tempo bonus.
namespace {
constexpr Value V_PAWN   = 100;
constexpr Value V_KNIGHT = 320;
constexpr Value V_BISHOP = 330;
constexpr Value V_ROOK   = 500;
constexpr Value V_QUEEN  = 900;

inline int diff(const Board& b, PieceType pt) {
    return b.pieces(pt, Color::WHITE).count() - b.pieces(pt, Color::BLACK).count();
}
} // namespace

Value evaluate(const Board& board, Value tempo) {
    int score = V_PAWN * diff(board, PieceType::PAWN) + V_KNIGHT * diff(board, PieceType::KNIGHT) +
                V_BISHOP * diff(board, PieceType::BISHOP) + V_ROOK * diff(board, PieceType::ROOK) +
                V_QUEEN * diff(board, PieceType::QUEEN);

    // Flip to the side-to-move's perspective (negamax convention).
    Value stm = (board.sideToMove() == Color::WHITE) ? score : -score;
    return stm + tempo;
}

} // namespace engine
