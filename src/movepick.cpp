#include "movepick.hpp"

#include "chess.hpp"
#include "see.hpp"

using namespace chess;

namespace engine {

namespace {

// Ordering-score buckets, spaced so the categories never interleave. An MVV-LVA
// score fits inside a bucket (max ~ 16 * 900 = 14400, well under the spacing).
constexpr int SCORE_TT       = 1 << 24;
constexpr int SCORE_GOOD_CAP = 1 << 20;
constexpr int SCORE_QUIET    = 0;
constexpr int SCORE_BAD_CAP  = -(1 << 20);

int scoreMove(const Board& board, Move move, Move ttMove) {
    if (move == ttMove)
        return SCORE_TT;
    if (board.isCapture(move)) {
        const int mvv = mvvLva(board, move);
        return seeGE(board, move, 0) ? SCORE_GOOD_CAP + mvv : SCORE_BAD_CAP + mvv;
    }
    return SCORE_QUIET;
}

// Stable insertion sort of `moves` by descending `scores` (move lists are short, so
// this beats a general sort and keeps generation order among equal-scored moves).
void sortByScore(Movelist& moves, int* scores, int n) {
    for (int i = 1; i < n; ++i) {
        const Move m = moves[i];
        const int  s = scores[i];
        int        j = i - 1;
        while (j >= 0 && scores[j] < s) {
            moves[j + 1]  = moves[j];
            scores[j + 1] = scores[j];
            --j;
        }
        moves[j + 1]  = m;
        scores[j + 1] = s;
    }
}

} // namespace

int mvvLva(const Board& board, Move move) {
    const PieceType victim = move.typeOf() == Move::ENPASSANT ? PieceType::PAWN
                                                              : board.at<PieceType>(move.to());
    const PieceType attacker = board.at<PieceType>(move.from());
    // Victim dominates; the attacker breaks ties so the cheapest attacker goes first.
    return 16 * pieceValue(victim) - pieceValue(attacker);
}

void orderMoves(const Board& board, Movelist& moves, Move ttMove) {
    int       scores[constants::MAX_MOVES];
    const int n = moves.size();
    for (int i = 0; i < n; ++i)
        scores[i] = scoreMove(board, moves[i], ttMove);
    sortByScore(moves, scores, n);
}

void orderCaptures(const Board& board, Movelist& moves) {
    int       scores[constants::MAX_MOVES];
    const int n = moves.size();
    for (int i = 0; i < n; ++i)
        scores[i] = mvvLva(board, moves[i]);
    sortByScore(moves, scores, n);
}

} // namespace engine
