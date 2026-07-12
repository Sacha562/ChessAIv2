#include "history.hpp"

#include "chess.hpp"

#include <cstdlib>
#include <cstring>

using namespace chess;

namespace engine {

namespace {

// Per-cutoff bonus scale: bonus = min(HIST_BONUS_CAP, HIST_BONUS_MULT * depth^2).
// A deeper cutoff is more trustworthy, so it moves the history entry further. Both
// are first-cut values; they are SPSA-tunable search params later (PLAN.md §18).
constexpr int HIST_BONUS_MULT = 16;
constexpr int HIST_BONUS_CAP  = 2000;

int historyBonus(int depth) {
    const int bonus = HIST_BONUS_MULT * depth * depth;
    return bonus < HIST_BONUS_CAP ? bonus : HIST_BONUS_CAP;
}

// Gravity update: nudge `entry` toward +/-MAX_HISTORY by `bonus`, damped by how close
// it already is to the bound. With |bonus| <= MAX_HISTORY this keeps `entry` inside
// [-MAX_HISTORY, MAX_HISTORY] forever, so history never overflows its int16 slot or
// its move-ordering score band. `entry` promotes to int in the arithmetic.
void applyBonus(int16_t& entry, int bonus) {
    const int clamped = bonus > MAX_HISTORY    ? MAX_HISTORY
                        : bonus < -MAX_HISTORY ? -MAX_HISTORY
                                               : bonus;
    entry += static_cast<int16_t>(clamped - entry * std::abs(clamped) / MAX_HISTORY);
}

} // namespace

void History::clear() {
    std::memset(butterfly_, 0, sizeof(butterfly_));
    std::memset(continuation_->v, 0, sizeof(continuation_->v));
    const Move none = Move(Move::NO_MOVE);
    for (auto& perPly : killers_) {
        perPly[0] = none;
        perPly[1] = none;
    }
    for (auto& c : counters_)
        c = none;
}

int History::quietScore(Color stm, Move move) const {
    if (!useHistory_) return 0;
    return butterfly_[stm][move.from().index()][move.to().index()];
}

int History::continuationScore(const ContHistContext& ctx, int movedPiece, int to) const {
    if (!useContHist_) return 0;
    int score = 0;
    if (ctx.piece1 >= 0) score += continuation_->v[ctx.piece1][ctx.to1][movedPiece][to];
    if (ctx.piece2 >= 0) score += continuation_->v[ctx.piece2][ctx.to2][movedPiece][to];
    return score;
}

void History::updateContinuation(const ContHistContext& ctx, int movedPiece, int to, int bonus) {
    if (ctx.piece1 >= 0) applyBonus(continuation_->v[ctx.piece1][ctx.to1][movedPiece][to], bonus);
    if (ctx.piece2 >= 0) applyBonus(continuation_->v[ctx.piece2][ctx.to2][movedPiece][to], bonus);
}

Move History::killer(int ply, int slot) const {
    if (!useKillers_) return Move(Move::NO_MOVE);
    return killers_[ply][slot];
}

int History::counterIndex(const Board& board, Move prevMove) {
    if (prevMove == Move(Move::NO_MOVE)) return -1;
    const Piece moved = board.at(prevMove.to());
    if (moved == Piece::NONE) return -1;
    return static_cast<int>(moved) * NUM_SQUARES + prevMove.to().index();
}

Move History::counter(const Board& board, Move prevMove) const {
    if (!useCounter_) return Move(Move::NO_MOVE);
    const int idx = counterIndex(board, prevMove);
    return idx < 0 ? Move(Move::NO_MOVE) : counters_[idx];
}

void History::updateQuietCutoff(const Board& board, Color stm, int ply, int depth, Move prevMove,
                                Move cutoff, const Move* quietsTried, int nTried,
                                const ContHistContext& ctx) {
    const int bonus = historyBonus(depth);

    // Reward the move that cut; penalise the quiets that were tried first and did not.
    // Both butterfly (from,to) and continuation (prior-move-conditioned piece,to) tables
    // learn from the same reward/malus, so the two ordering signals stay symmetric.
    applyBonus(butterfly_[stm][cutoff.from().index()][cutoff.to().index()], bonus);
    if (useContHist_)
        updateContinuation(ctx, static_cast<int>(board.at(cutoff.from())), cutoff.to().index(),
                           bonus);
    for (int i = 0; i < nTried; ++i) {
        const Move m = quietsTried[i];
        applyBonus(butterfly_[stm][m.from().index()][m.to().index()], -bonus);
        if (useContHist_)
            updateContinuation(ctx, static_cast<int>(board.at(m.from())), m.to().index(), -bonus);
    }

    // Killers: keep the two most recent distinct cutoff moves, newest in slot 0.
    if (killers_[ply][0] != cutoff) {
        killers_[ply][1] = killers_[ply][0];
        killers_[ply][0] = cutoff;
    }

    // Countermove: this quiet refutes the opponent's previous move.
    const int idx = counterIndex(board, prevMove);
    if (idx >= 0) counters_[idx] = cutoff;
}

} // namespace engine
