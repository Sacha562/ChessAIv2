#include "movepick.hpp"

#include "chess.hpp"
#include "history.hpp"
#include "see.hpp"

using namespace chess;

namespace engine {

namespace {

// Widest the history-scored quiet band can reach: butterfly (+/-MAX_HISTORY) plus one
// continuation-history entry per prior ply, each also bounded to +/-MAX_HISTORY.
constexpr int QUIET_BAND = (1 + CONT_PLIES) * MAX_HISTORY;

// Ordering-score buckets, spaced so the categories never interleave. A capture's
// MVV-LVA score fits inside its bucket (max ~ 16 * 900 = 14400) and a quiet's
// butterfly + continuation history score fits inside the quiet band (+/-QUIET_BAND).
constexpr int SCORE_TT       = 1 << 24;
constexpr int SCORE_GOOD_CAP = 1 << 20;
constexpr int SCORE_KILLER1  = 1 << 19;
constexpr int SCORE_KILLER2  = 1 << 18;
constexpr int SCORE_COUNTER  = 1 << 17;
constexpr int SCORE_QUIET    = 0;
constexpr int SCORE_BAD_CAP  = -(1 << 20);

// The killer / countermove tiers must sit strictly above the history-scored quiet
// band, and that band strictly above the losing-capture bucket, or the categories
// would interleave once history saturates.
static_assert(SCORE_COUNTER > QUIET_BAND, "history band overlaps the countermove tier");
static_assert(SCORE_QUIET - QUIET_BAND > SCORE_BAD_CAP + 16 * 900,
              "history band overlaps the losing-capture bucket");

// Quiet-move ordering signals precomputed once per node (identical for every quiet).
struct QuietOrder {
    const History*         hist;
    Color                  stm;
    Move                   killer1;
    Move                   killer2;
    Move                   counter;
    const ContHistContext* cont;
};

int scoreMove(const Board& board, Move move, Move ttMove, const QuietOrder& q) {
    if (move == ttMove) return SCORE_TT;
    if (board.isCapture(move)) {
        const int mvv = mvvLva(board, move);
        return seeGE(board, move, 0) ? SCORE_GOOD_CAP + mvv : SCORE_BAD_CAP + mvv;
    }
    if (move == q.killer1) return SCORE_KILLER1;
    if (move == q.killer2) return SCORE_KILLER2;
    if (move == q.counter) return SCORE_COUNTER;
    const int cont = q.hist->continuationScore(*q.cont, static_cast<int>(board.at(move.from())),
                                               move.to().index());
    return SCORE_QUIET + q.hist->quietScore(q.stm, move) + cont;
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
    const PieceType victim =
        move.typeOf() == Move::ENPASSANT ? PieceType::PAWN : board.at<PieceType>(move.to());
    const PieceType attacker = board.at<PieceType>(move.from());
    // Victim dominates; the attacker breaks ties so the cheapest attacker goes first.
    return 16 * pieceValue(victim) - pieceValue(attacker);
}

void orderMoves(const Board& board, Movelist& moves, Move ttMove, const History& hist, int ply,
                Move prevMove, const ContHistContext& contCtx) {
    // Fetch the quiet signals once — they are the same for every move at this node.
    const QuietOrder q{&hist,
                       board.sideToMove(),
                       hist.killer(ply, 0),
                       hist.killer(ply, 1),
                       hist.counter(board, prevMove),
                       &contCtx};

    int       scores[constants::MAX_MOVES];
    const int n = moves.size();
    for (int i = 0; i < n; ++i)
        scores[i] = scoreMove(board, moves[i], ttMove, q);
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
