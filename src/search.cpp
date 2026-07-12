#include "search.hpp"
#include "eval.hpp"
#include "movepick.hpp"
#include "see.hpp"
#include "tt.hpp"
#include "chess.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

using namespace chess;

namespace engine {

namespace {
constexpr int64_t MOVE_OVERHEAD_MS = 30;
constexpr int64_t TIME_CHECK_MASK  = 2047; // check clock every 2048 nodes

// --- Selective-search knobs (Phase 1b). First-cut values; SPSA-tunable later. ---

// Internal Iterative Reduction: at a node this deep with no TT move, search one ply
// shallower rather than pay a full search behind a poor first move.
constexpr int IIR_MIN_DEPTH = 4;

// Null-move pruning: give the opponent a free move at reduced depth; a position still
// >= beta is assumed too good and pruned. R grows with depth and the eval margin (the
// base and eval divisor are tunable — Tunables::nmpBase / nmpEvalDiv).
constexpr int NMP_MIN_DEPTH = 3;
constexpr int NMP_DIV       = 3;
constexpr int NMP_EVAL_MAX  = 3;

// Depth gates for the shallow forward-pruning trio (their margins are tunable Tunables
// fields: rfpMargin, futMargin/futBase, lmpBase).
constexpr int RFP_MAX_DEPTH = 6;
constexpr int FUT_MAX_DEPTH = 6;
constexpr int LMP_MAX_DEPTH = 6;

// Late move reductions: reduce late quiets; re-search if one beats alpha. The reduction
// curve (base/divisor) is tunable — see Searcher::buildReductions.
constexpr int LMR_MIN_DEPTH = 3;
constexpr int LMR_MIN_MOVES = 2; // start reducing at the 3rd move

// Aspiration windows: only worthwhile once a few iterations have stabilised.
constexpr int ASPIRATION_MIN_DEPTH = 5;

// Singular extensions: the TT entry must be nearly as deep as this node for its score
// to be a trustworthy singular-verification target (tt.depth >= depth - this). The
// verification runs at (depth - 1) / 2. Structural gate; the margins are tunable.
constexpr int SINGULAR_TT_DEPTH_MARGIN = 3;

// A TT entry may only cut when the fifty-move clock is well below the 100-halfmove
// draw; otherwise a stored score can't be trusted (it may ignore the looming draw).
constexpr int TT_CUTOFF_MAX_HALFMOVE = 90;

// Quiet move-count threshold for LMP at `depth`; fewer when the eval is not improving.
int lmpCount(int depth, bool improving, int base) {
    const int count = base + depth * depth;
    return improving ? count : count / 2;
}

// Does `stm` have any piece heavier than a pawn? Zugzwang guard for null-move pruning.
bool hasNonPawnMaterial(const Board& board, Color stm) {
    return !(board.pieces(PieceType::KNIGHT, stm) | board.pieces(PieceType::BISHOP, stm) |
             board.pieces(PieceType::ROOK, stm) | board.pieces(PieceType::QUEEN, stm))
                .empty();
}

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

// (Re)build the LMR reduction table from the tunable curve
// r(depth, moveCount) ~= lmrBase/100 + ln(depth)*ln(moveCount)/(lmrDivisor/100).
// Called once per search (the base/divisor are UCI-tunable); indices saturate at
// LMR_DIM, where reductions plateau. Row/column 0 stay 0 (never indexed).
void Searcher::buildReductions() {
    const double base = tp_.lmrBase / 100.0;
    const double div  = std::max(1, tp_.lmrDivisor) / 100.0;
    for (int d = 1; d < LMR_DIM; ++d)
        for (int m = 1; m < LMR_DIM; ++m)
            reductions_[d][m] = static_cast<uint8_t>(base + std::log(d) * std::log(m) / div);
}

int Searcher::lmrReduction(int depth, int moveCount) const {
    return reductions_[std::min(depth, LMR_DIM - 1)][std::min(moveCount, LMR_DIM - 1)];
}

Value Searcher::search(Board& board, int depth, Value alpha, Value beta, int ply, Move prevMove,
                       Move excluded) {
    if (timeUp_) return VALUE_ZERO;

    const bool inSingular = excluded != Move(Move::NO_MOVE);

    ++nodes_;
    if ((nodes_ & TIME_CHECK_MASK) == 0) checkStop();

    const bool pvNode = beta - alpha > 1; // wide window == a principal-variation node

    // Draw detection (never at the root — we must return a move there).
    // isRepetition(1) = twofold: treat the first in-search repetition as a draw
    // (standard engine convention; the library's default arg is threefold).
    if (ply > 0 &&
        (board.isHalfMoveDraw() || board.isInsufficientMaterial() || board.isRepetition(1)))
        return VALUE_DRAW;

    // Leaf, or the ply guard that keeps per-ply tables (killers, static evals) in
    // bounds under check extensions: resolve captures before evaluating.
    if (depth <= 0 || ply >= MAX_PLY - 1) return qsearch(board, alpha, beta, ply);

    const uint64_t key       = board.hash();
    const Value    alphaOrig = alpha;

    // Transposition table probe: a deep-enough entry can cut immediately (never at a
    // PV node, which includes the root), and its move seeds move ordering.
    Move          ttMove = Move(Move::NO_MOVE);
    const TTProbe tt     = tt_.probe(key);
    if (tt.hit) {
        ttMove = tt.move;
        // Never take the TT cutoff inside a singular-verification search: it would just
        // return the excluded move's own score instead of searching the alternatives.
        if (!pvNode && !inSingular && tt.depth >= depth &&
            board.halfMoveClock() < TT_CUTOFF_MAX_HALFMOVE) {
            const Value ttv = valueFromTT(tt.value, ply);
            if (tt.bound == BOUND_EXACT || (tt.bound == BOUND_LOWER && ttv >= beta) ||
                (tt.bound == BOUND_UPPER && ttv <= alpha))
                return ttv;
        }
    }

    const bool inCheck = board.inCheck();

    // Static eval (used by the whole-node pruning below and the `improving` trend).
    // Reuse the TT's stored static eval when present; skip it entirely when in check.
    Value eval;
    if (inCheck) {
        eval              = VALUE_NONE;
        staticEvals_[ply] = VALUE_NONE;
    } else {
        eval = (tt.hit && tt.eval != VALUE_NONE) ? tt.eval : evaluate(board, tp_.tempo);
        staticEvals_[ply] = eval;
    }
    const bool improving =
        !inCheck && ply >= 2 && staticEvals_[ply - 2] != VALUE_NONE && eval > staticEvals_[ply - 2];

    // Whole-node forward pruning: only off the PV, out of check, and away from mate
    // scores (where every ply matters).
    if (!pvNode && !inCheck && std::abs(beta) < VALUE_MATE_IN_MAX_PLY) {
        // Reverse futility pruning (static null-move): a static eval this far above
        // beta near the horizon is assumed to hold.
        if (tp_.useRfp && depth <= RFP_MAX_DEPTH && eval - tp_.rfpMargin * depth >= beta)
            return eval;

        // Null-move pruning: skip a turn; if the position is still >= beta at reduced
        // depth it is too good, so prune. Guarded against zugzwang (needs non-pawn
        // material) and a below-beta eval.
        if (tp_.useNmp && depth >= NMP_MIN_DEPTH && eval >= beta &&
            hasNonPawnMaterial(board, board.sideToMove())) {
            const int R = tp_.nmpBase + depth / NMP_DIV +
                          std::min<int>((eval - beta) / tp_.nmpEvalDiv, NMP_EVAL_MAX);
            contPiece_[ply] = -1; // a null move leaves the child no continuation context
            board.makeNullMove();
            const Value nullScore =
                -search(board, depth - 1 - R, -beta, -beta + 1, ply + 1, Move(Move::NO_MOVE));
            board.unmakeNullMove();
            if (timeUp_) return VALUE_ZERO;
            if (nullScore >= beta) return is_mate(nullScore) ? beta : nullScore;
        }
    }

    // Internal Iterative Reduction: with no hash move, ordering is unreliable, so
    // search shallower and let the reduced search supply a TT move for next time.
    if (tp_.useIir && depth >= IIR_MIN_DEPTH && ttMove == Move(Move::NO_MOVE)) --depth;

    Movelist moves;
    movegen::legalmoves(moves, board);

    if (moves.empty()) return inCheck ? mated_in(ply) : VALUE_DRAW;

    // Resolve the continuation-history context — the (piece, to) of the moves 1 and 2
    // plies back, recorded in the search stack as those moves were made. Absent at the
    // root, after a null move (-1), or before enough plies exist.
    ContHistContext contCtx;
    if (ply >= 1 && contPiece_[ply - 1] >= 0) {
        contCtx.piece1 = contPiece_[ply - 1];
        contCtx.to1    = contTo_[ply - 1];
    }
    if (ply >= 2 && contPiece_[ply - 2] >= 0) {
        contCtx.piece2 = contPiece_[ply - 2];
        contCtx.to2    = contTo_[ply - 2];
    }

    // Order: TT move, then winning/equal captures (SEE >= 0), killers, countermove,
    // history + continuation-history quiets, then losing captures. The biggest speed lever.
    orderMoves(board, moves, ttMove, history_, ply, prevMove, contCtx);

    Value best      = -VALUE_INFINITE;
    Move  bestMove  = Move(Move::NO_MOVE);
    int   moveCount = 0;
    Move  quietsTried[constants::MAX_MOVES];
    int   nQuietsTried = 0;
    for (const auto& m : moves) {
        if (m == excluded) continue; // singular verification: search everything but the TT move
        ++moveCount;
        const bool isQuiet    = !board.isCapture(m);
        const int  movedPiece = static_cast<int>(board.at(m.from())); // colour+type, pre-move

        // Move-loop pruning of quiets: off the PV, out of check, once a real move has
        // been searched, and not while being mated (we still need an escape).
        if (!pvNode && !inCheck && isQuiet && moveCount > 1 && best > VALUE_MATED_IN_MAX_PLY) {
            // Late move pruning: past a depth-scaled quiet count, skip the rest.
            if (tp_.useLmp && depth <= LMP_MAX_DEPTH &&
                moveCount >= lmpCount(depth, improving, tp_.lmpBase))
                continue;
            // Futility pruning: a quiet the static eval plus a margin cannot lift to
            // alpha near the horizon.
            if (tp_.useFutility && depth <= FUT_MAX_DEPTH &&
                eval + tp_.futMargin * depth + tp_.futBase <= alpha)
                continue;
        }

        // Singular extension: decided before the move is made, for the TT move only. If
        // the TT move looks forced — a deep-enough lower-bound TT score — verify it with
        // a reduced-depth search that excludes it and searches against a lowered beta. If
        // every alternative fails below that beta the TT move is singular and searched
        // deeper (doubly so when they fail far below); if it is not singular yet the TT
        // score already beats beta, reduce it instead (a multi-cut-style negative
        // extension). Never inside a verification search, and never on a mate score.
        int singularExt = 0;
        if (tp_.useSingular && !inSingular && m == ttMove && ply > 0 && tt.hit &&
            depth >= tp_.singularMinDepth && tt.depth >= depth - SINGULAR_TT_DEPTH_MARGIN &&
            (tt.bound == BOUND_LOWER || tt.bound == BOUND_EXACT) && ply + 1 < MAX_PLY - 1) {
            const Value ttValue = valueFromTT(tt.value, ply);
            if (!is_mate(ttValue)) {
                const Value sBeta = std::max<Value>(ttValue - tp_.singularMargin * depth,
                                                    VALUE_MATED_IN_MAX_PLY + 1);
                const Value sScore =
                    search(board, (depth - 1) / 2, sBeta - 1, sBeta, ply, prevMove, ttMove);
                if (timeUp_) return VALUE_ZERO;
                if (sScore < sBeta)
                    singularExt = (!pvNode && sScore < sBeta - tp_.singularDoubleMargin) ? 2 : 1;
                else if (ttValue >= beta)
                    singularExt = -1;
            }
        }

        tt_.prefetch(board.zobristAfter(m));
        contPiece_[ply] = movedPiece; // search-stack record for the child's continuation history
        contTo_[ply]    = m.to().index();
        board.makeMove(m);
        const bool givesCheck = board.inCheck();

        // Extensions: the singular extension decided above plus a check extension for a
        // checking reply. Bounded so ply can never overrun the per-ply tables.
        const int checkExt = (tp_.useCheckExt && givesCheck && ply + 1 < MAX_PLY - 1) ? 1 : 0;
        const int newDepth = depth - 1 + singularExt + checkExt;

        // Late move reductions: search late quiets shallower, less so on the PV or when
        // the eval is improving. A reduced search that beats alpha is re-searched.
        int reduction = 0;
        if (tp_.useLmr && depth >= LMR_MIN_DEPTH && moveCount > LMR_MIN_MOVES && isQuiet &&
            !givesCheck) {
            reduction = lmrReduction(depth, moveCount);
            if (pvNode) --reduction;
            if (improving) --reduction;
            reduction = std::clamp(reduction, 0, newDepth - 1);
        }

        // Principal Variation Search with the LMR reduction folded in: full window for
        // the first move; a null-window scout (reduced for late quiets) for the rest,
        // re-searched at full depth on a surprise and at the full window for a new PV.
        Value score;
        if (moveCount == 1) {
            score = -search(board, newDepth, -beta, -alpha, ply + 1, m);
        } else {
            score = -search(board, newDepth - reduction, -alpha - 1, -alpha, ply + 1, m);
            if (reduction > 0 && score > alpha)
                score = -search(board, newDepth, -alpha - 1, -alpha, ply + 1, m);
            if (score > alpha && score < beta)
                score = -search(board, newDepth, -beta, -alpha, ply + 1, m);
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
            // penalise the quiets tried before it, and record killer / countermove /
            // continuation history.
            if (isQuiet)
                history_.updateQuietCutoff(board, board.sideToMove(), ply, depth, prevMove, m,
                                           quietsTried, nQuietsTried, contCtx);
            break;
        }
        if (isQuiet) quietsTried[nQuietsTried++] = m;
    }

    // Don't store a singular-verification result: it is the value of a restricted move
    // set (the TT move excluded), not of this position, so it must not pollute the TT.
    if (!inSingular) {
        const Bound bound = best >= beta       ? BOUND_LOWER
                            : best > alphaOrig ? BOUND_EXACT
                                               : BOUND_UPPER;
        tt_.store(key, depth, valueToTT(best, ply), inCheck ? VALUE_NONE : eval, bestMove, bound);
    }

    return best;
}

// Aspiration windows: re-search each root iteration inside a narrow window around the
// previous score to win more cutoffs. On a fail low/high, widen only the failing side
// exponentially and re-search; below the gate depth (or on a mate score) fall back to a
// full window. Returns the in-window score (or the last score on time-out).
Value Searcher::aspirationSearch(Board& board, int depth, Value prevScore) {
    if (!tp_.useAspiration || depth < ASPIRATION_MIN_DEPTH || is_mate(prevScore))
        return search(board, depth, -VALUE_INFINITE, VALUE_INFINITE, 0, Move(Move::NO_MOVE));

    Value delta = tp_.aspirationDelta;
    Value alpha = std::max<Value>(prevScore - delta, -VALUE_INFINITE);
    Value beta  = std::min<Value>(prevScore + delta, VALUE_INFINITE);

    while (true) {
        const Value v = search(board, depth, alpha, beta, 0, Move(Move::NO_MOVE));
        if (timeUp_) return v;
        if (v <= alpha) { // fail low: relax alpha, pull beta toward the middle
            beta  = (alpha + beta) / 2;
            alpha = std::max<Value>(v - delta, -VALUE_INFINITE);
        } else if (v >= beta) { // fail high: relax beta
            beta = std::min<Value>(v + delta, VALUE_INFINITE);
        } else {
            return v; // inside the window
        }
        delta += delta; // widen the failing side exponentially
    }
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
        orderMoves(board, moves, Move(Move::NO_MOVE), history_, ply, Move(Move::NO_MOVE),
                   ContHistContext{});
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
    history_.setEnabled(tp_.useKillers, tp_.useHistory, tp_.useCountermove, tp_.useContHist);
    buildReductions();
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

    // Cap the root depth at MAX_PLY - 1 so a huge `go depth N` can't run the
    // iterative-deepening loop itself past the per-ply tables. (A node's `ply` can still
    // exceed the root depth via check extensions; what keeps the killer / staticEvals_
    // tables in bounds there is the `ply >= MAX_PLY - 1` leaf guard in search/qsearch,
    // plus the `ply + 1 < MAX_PLY - 1` gate on the extension itself.)
    const int maxDepth = std::min((limits.depth > 0) ? limits.depth : MAX_PLY - 1, MAX_PLY - 1);
    Value     score    = VALUE_ZERO;

    for (int depth = 1; depth <= maxDepth; ++depth) {
        rootBest_ = Move(Move::NO_MOVE);
        Value v   = aspirationSearch(board, depth, score);

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
