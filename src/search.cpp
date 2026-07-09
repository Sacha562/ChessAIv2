#include "search.hpp"
#include "eval.hpp"
#include "movepick.hpp"
#include "see.hpp"
#include "tt.hpp"
#include "chess.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

using namespace chess;

namespace engine {

namespace {
constexpr int64_t MOVE_OVERHEAD_MS = 30;
constexpr int64_t TIME_CHECK_MASK  = 2047; // check clock every 2048 nodes

// Internal Iterative Reduction: at a node this deep with no TT move to lead ordering,
// search one ply shallower rather than pay for a full-depth search behind a poor first
// move — the reduced search leaves a TT move behind for the re-search. SPSA-tunable.
constexpr int IIR_MIN_DEPTH = 4;

// Format a score for a UCI `info ... score` field.
std::string scoreToUci(Value v) {
    std::ostringstream ss;
    if (is_mate(v)) {
        int mate = (v > 0) ? (VALUE_MATE - v + 1) / 2 : -(VALUE_MATE + v) / 2;
        ss << "mate " << mate;
    } else {
        ss << "cp " << v;
    }
    return ss.str();
}
} // namespace

int64_t Searcher::elapsedMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - start_).count();
}

bool Searcher::checkStop() {
    if (stop_.load(std::memory_order_relaxed)) {
        timeUp_ = true;
        return true;
    }
    if (nodeLimit_ && nodes_ >= static_cast<uint64_t>(nodeLimit_)) {
        timeUp_ = true;
        return true;
    }
    if (useTime_ && elapsedMs() >= hardLimitMs_) {
        timeUp_ = true;
        return true;
    }
    return false;
}

// Derive the node cap and soft/hard clock budgets for this `go`. Sets `useTime_`
// only when a wall clock actually governs the move; explicit depth/nodes/infinite
// searches run untimed.
void Searcher::setupTiming(const Limits& limits, const Board& board) {
    nodeLimit_   = limits.nodes;
    useTime_     = false;
    softLimitMs_ = std::numeric_limits<int64_t>::max();
    hardLimitMs_ = std::numeric_limits<int64_t>::max();

    // `movetime`: spend exactly this budget; soft and hard coincide.
    if (limits.movetime > 0) {
        useTime_     = true;
        softLimitMs_ = std::max<int64_t>(1, limits.movetime - MOVE_OVERHEAD_MS);
        hardLimitMs_ = softLimitMs_;
        return;
    }

    // A depth/node/infinite search has no clock budget.
    if (limits.infinite || limits.depth > 0 || limits.nodes > 0) return;

    const bool    white     = (board.sideToMove() == Color::WHITE);
    const int64_t remaining = white ? limits.wtime : limits.btime;
    const int64_t inc       = white ? limits.winc : limits.binc;
    if (remaining <= 0) return; // no clock info -> untimed

    useTime_           = true;
    const int     mtg  = limits.movestogo > 0 ? limits.movestogo : tp_.assumedMovestogo;
    const int64_t base = remaining / mtg + inc / 2;

    int64_t soft = base * tp_.softPermille / 1000;
    int64_t hard = base * tp_.hardPermille / 1000;

    // Never flag: cap the hard limit at half the clock (less overhead), then keep
    // the soft limit at or below it.
    const int64_t cap = std::min<int64_t>(remaining / 2, remaining - MOVE_OVERHEAD_MS);
    hard              = std::min(hard, std::max<int64_t>(1, cap));
    soft              = std::min(soft, hard);

    softLimitMs_ = std::max<int64_t>(1, soft);
    hardLimitMs_ = std::max<int64_t>(1, hard);
}

Value Searcher::search(Board& board, int depth, Value alpha, Value beta, int ply, Move prevMove) {
    if (timeUp_) return VALUE_ZERO;

    ++nodes_;
    if ((nodes_ & TIME_CHECK_MASK) == 0) checkStop();

    // Draw detection (never at the root — we must return a move there).
    // isRepetition(1) = twofold: treat the first in-search repetition as a draw
    // (standard engine convention; the library's default arg is threefold).
    if (ply > 0 &&
        (board.isHalfMoveDraw() || board.isInsufficientMaterial() || board.isRepetition(1)))
        return VALUE_DRAW;

    if (depth <= 0) return qsearch(board, alpha, beta, ply); // resolve captures before evaluating

    const uint64_t key       = board.hash();
    const Value    alphaOrig = alpha;

    // Transposition table probe: a deep-enough entry can cut immediately (never at
    // the root, where we must return a move), and its move seeds move ordering.
    Move          ttMove = Move(Move::NO_MOVE);
    const TTProbe tt     = tt_.probe(key);
    if (tt.hit) {
        ttMove = tt.move;
        if (ply > 0 && tt.depth >= depth && board.halfMoveClock() < 90) {
            const Value ttv = valueFromTT(tt.value, ply);
            if (tt.bound == BOUND_EXACT || (tt.bound == BOUND_LOWER && ttv >= beta) ||
                (tt.bound == BOUND_UPPER && ttv <= alpha))
                return ttv;
        }
    }

    // Internal Iterative Reduction: with no hash move, ordering is unreliable, so
    // search shallower and let the reduced search supply a TT move for next time.
    if (depth >= IIR_MIN_DEPTH && ttMove == Move(Move::NO_MOVE)) --depth;

    Movelist moves;
    movegen::legalmoves(moves, board);

    if (moves.empty()) return board.inCheck() ? mated_in(ply) : VALUE_DRAW;

    // Order: TT move, then winning/equal captures (SEE >= 0), killers, countermove,
    // history-scored quiets, then losing captures. The single biggest lever on speed.
    orderMoves(board, moves, ttMove, history_, ply, prevMove);

    Value best      = -VALUE_INFINITE;
    Move  bestMove  = Move(Move::NO_MOVE);
    int   moveCount = 0;
    Move  quietsTried[constants::MAX_MOVES];
    int   nQuietsTried = 0;
    for (const auto& m : moves) {
        ++moveCount;
        const bool isQuiet = !board.isCapture(m);
        tt_.prefetch(board.zobristAfter(m));
        board.makeMove(m);

        // Principal Variation Search: search the first (best-ordered) move with the
        // full window to establish the PV, then probe each later move with a
        // null-window scout `(alpha, alpha+1)` — far more cutoffs, so refuting an
        // inferior move is cheap. A scout that raises alpha *and* stays below beta
        // means the ordering guess was wrong; re-search that move on the full window
        // to get its true score. (When beta == alpha+1 the scout already *is* the
        // full window, so the guard skips a redundant re-search.)
        Value score;
        if (moveCount == 1) {
            score = -search(board, depth - 1, -beta, -alpha, ply + 1, m);
        } else {
            score = -search(board, depth - 1, -alpha - 1, -alpha, ply + 1, m);
            if (score > alpha && score < beta)
                score = -search(board, depth - 1, -beta, -alpha, ply + 1, m);
        }

        board.unmakeMove(m);

        if (timeUp_) return VALUE_ZERO; // abort: discard partial result (no TT store)

        if (score > best) {
            best     = score;
            bestMove = m;
            if (ply == 0) rootBest_ = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            // Fail-high cutoff. A quiet cutoff teaches move ordering: reward this move,
            // penalise the quiets tried before it, and record killer / countermove.
            if (isQuiet)
                history_.updateQuietCutoff(board, board.sideToMove(), ply, depth, prevMove, m,
                                           quietsTried, nQuietsTried);
            break;
        }
        if (isQuiet) quietsTried[nQuietsTried++] = m;
    }

    const Bound bound = best >= beta ? BOUND_LOWER : best > alphaOrig ? BOUND_EXACT : BOUND_UPPER;
    tt_.store(key, depth, valueToTT(best, ply), VALUE_NONE, bestMove, bound);

    return best;
}

// Quiescence search: at the horizon, keep resolving captures (and, when in check,
// all evasions) until the position is quiet, so the static eval is never read in the
// middle of a capture sequence (the horizon effect). Fail-soft; no TT of its own.
Value Searcher::qsearch(Board& board, Value alpha, Value beta, int ply) {
    if (timeUp_) return VALUE_ZERO;

    ++nodes_;
    if ((nodes_ & TIME_CHECK_MASK) == 0) checkStop();

    // Draw detection (always below the root here) — same rule as the main search.
    if (board.isHalfMoveDraw() || board.isInsufficientMaterial() || board.isRepetition(1))
        return VALUE_DRAW;

    const bool inCheck = board.inCheck();

    // Stand-pat: the static eval is a lower bound on the score — but standing pat is
    // forbidden in check, where the side to move must answer the threat.
    Value best = -VALUE_INFINITE;
    if (!inCheck) {
        best = evaluate(board, tp_.tempo);
        if (best >= beta) return best; // fail-soft beta cutoff
        if (best > alpha) alpha = best;
    }
    if (ply >= MAX_PLY - 1)
        return inCheck ? evaluate(board, tp_.tempo) : best; // depth guard: stop descending

    // In check: search every evasion, ordered like a main node. Otherwise: only
    // captures / capture-promotions, MVV-LVA ordered.
    Movelist moves;
    if (inCheck) {
        movegen::legalmoves(moves, board);
        if (moves.empty()) return mated_in(ply); // checkmate
        orderMoves(board, moves, Move(Move::NO_MOVE), history_, ply, Move(Move::NO_MOVE));
    } else {
        // Not in check: only captures. An empty list here means "no captures", not
        // necessarily "no moves" — so a stalemate is scored as its (quiet) stand-pat
        // eval rather than a draw. This is the standard quiescence limitation (q-search
        // never enumerates quiets); the main search catches stalemate at depth > 0.
        movegen::legalmoves<movegen::MoveGenType::CAPTURE>(moves, board);
        orderCaptures(board, moves);
    }

    const Value standPat = best;
    const bool  useDelta = !inCheck && board.occ().count() > tp_.endgamePieces;

    for (const auto& m : moves) {
        if (!inCheck) {
            // Delta pruning: if even winning the captured piece (plus a cushion)
            // cannot lift stand-pat to alpha, skip it. Off in late endgames, where
            // such material swings decide the game.
            if (useDelta && m.typeOf() != Move::PROMOTION) {
                const PieceType victim =
                    m.typeOf() == Move::ENPASSANT ? PieceType::PAWN : board.at<PieceType>(m.to());
                if (standPat + pieceValue(victim) + tp_.deltaMargin <= alpha) continue;
            }
            // SEE pruning: never search a losing capture in quiescence.
            if (!seeGE(board, m, 0)) continue;
        }

        board.makeMove(m);
        const Value score = -qsearch(board, -beta, -alpha, ply + 1);
        board.unmakeMove(m);

        if (timeUp_) return VALUE_ZERO;

        if (score > best) {
            best = score;
            if (score > alpha) alpha = score;
            if (alpha >= beta) break; // fail-high cutoff
        }
    }

    return best;
}

Move Searcher::think(Board board, const Limits& limits, bool printBest, bool printInfo) {
    start_  = std::chrono::steady_clock::now();
    nodes_  = 0;
    timeUp_ = false;
    tt_.newSearch();
    rootBest_          = Move(Move::NO_MOVE);
    rootBestCompleted_ = Move(Move::NO_MOVE);

    // Guarantee a legal fallback so we never emit a null bestmove.
    {
        Movelist moves;
        movegen::legalmoves(moves, board);
        if (moves.empty()) {
            if (printBest) std::cout << "bestmove 0000" << std::endl;
            return Move(Move::NO_MOVE);
        }
        rootBestCompleted_ = moves[0];
    }

    setupTiming(limits, board);

    // Cap at MAX_PLY - 1: ply indexes fixed-size per-ply tables (killers), and with no
    // extensions yet a node's ply never exceeds the root depth. Clamp so a large
    // `go depth N` can never drive ply out of those tables.
    const int maxDepth = std::min((limits.depth > 0) ? limits.depth : MAX_PLY - 1, MAX_PLY - 1);
    Value     score    = VALUE_ZERO;

    for (int depth = 1; depth <= maxDepth; ++depth) {
        rootBest_ = Move(Move::NO_MOVE);
        Value v   = search(board, depth, -VALUE_INFINITE, VALUE_INFINITE, 0, Move(Move::NO_MOVE));

        if (timeUp_) break; // incomplete iteration: keep previous best

        score              = v;
        rootBestCompleted_ = rootBest_;

        if (printInfo) {
            const int64_t  ms  = elapsedMs();
            const uint64_t nps = ms > 0 ? (nodes_ * 1000ull / static_cast<uint64_t>(ms)) : nodes_;
            std::cout << "info depth " << depth << " score " << scoreToUci(score) << " nodes "
                      << nodes_ << " nps " << nps << " hashfull " << tt_.hashfull() << " time "
                      << ms << " pv " << uci::moveToUci(rootBestCompleted_) << std::endl;
        }

        // Soft stop: past the soft budget, a new (more expensive) depth won't
        // finish in time, so stop now and keep this iteration's move.
        if (useTime_ && elapsedMs() >= softLimitMs_) break;
        if (is_mate(score)) break; // forced mate found
    }

    if (printBest) std::cout << "bestmove " << uci::moveToUci(rootBestCompleted_) << std::endl;
    return rootBestCompleted_;
}

} // namespace engine
