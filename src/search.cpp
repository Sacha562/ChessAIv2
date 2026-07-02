#include "search.hpp"
#include "eval.hpp"
#include "chess.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

using namespace chess;

namespace engine {

namespace {
constexpr int64_t MOVE_OVERHEAD_MS  = 30;
constexpr int64_t TIME_CHECK_MASK   = 2047; // check clock every 2048 nodes

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
    if (stop_.load(std::memory_order_relaxed)) { timeUp_ = true; return true; }
    if (nodeLimit_ && nodes_ >= static_cast<uint64_t>(nodeLimit_)) { timeUp_ = true; return true; }
    if (useTime_ && elapsedMs() >= allocatedMs_) { timeUp_ = true; return true; }
    return false;
}

Value Searcher::search(Board& board, int depth, Value alpha, Value beta, int ply) {
    if (timeUp_) return VALUE_ZERO;

    ++nodes_;
    if ((nodes_ & TIME_CHECK_MASK) == 0) checkStop();

    // Draw detection (never at the root — we must return a move there).
    // isRepetition(1) = twofold: treat the first in-search repetition as a draw
    // (standard engine convention; the library's default arg is threefold).
    if (ply > 0 &&
        (board.isHalfMoveDraw() || board.isInsufficientMaterial() || board.isRepetition(1)))
        return VALUE_DRAW;

    if (depth <= 0)
        return evaluate(board);   // Phase 0: static leaf. Quiescence arrives in Phase 1a.

    Movelist moves;
    movegen::legalmoves(moves, board);

    if (moves.empty())
        return board.inCheck() ? mated_in(ply) : VALUE_DRAW;

    Value best = -VALUE_INFINITE;
    for (const auto& m : moves) {
        board.makeMove(m);
        Value score = -search(board, depth - 1, -beta, -alpha, ply + 1);
        board.unmakeMove(m);

        if (timeUp_) return VALUE_ZERO;   // abort: discard partial result

        if (score > best) {
            best = score;
            if (ply == 0) rootBest_ = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;         // fail-high cutoff
    }
    return best;
}

Move Searcher::think(Board board, const Limits& limits, bool printBest, bool printInfo) {
    start_   = std::chrono::steady_clock::now();
    nodes_   = 0;
    timeUp_  = false;
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

    // Configure limits.
    nodeLimit_   = limits.nodes;
    useTime_     = false;
    allocatedMs_ = std::numeric_limits<int64_t>::max();

    if (limits.movetime > 0) {
        useTime_     = true;
        allocatedMs_ = std::max<int64_t>(1, limits.movetime - MOVE_OVERHEAD_MS);
    } else if (!limits.infinite && limits.depth == 0 && limits.nodes == 0) {
        const bool white = (board.sideToMove() == Color::WHITE);
        const int64_t t   = white ? limits.wtime : limits.btime;
        const int64_t inc = white ? limits.winc  : limits.binc;
        if (t > 0) {
            useTime_ = true;
            const int mtg = limits.movestogo > 0 ? limits.movestogo : 30;
            int64_t alloc = t / mtg + inc / 2;
            alloc = std::min<int64_t>(alloc, t - MOVE_OVERHEAD_MS);
            allocatedMs_ = std::max<int64_t>(1, alloc);
        }
    }

    const int maxDepth = (limits.depth > 0) ? limits.depth : MAX_PLY - 1;
    Value score = VALUE_ZERO;

    for (int depth = 1; depth <= maxDepth; ++depth) {
        rootBest_ = Move(Move::NO_MOVE);
        Value v = search(board, depth, -VALUE_INFINITE, VALUE_INFINITE, 0);

        if (timeUp_) break;   // incomplete iteration: keep previous best

        score = v;
        rootBestCompleted_ = rootBest_;

        if (printInfo) {
            const int64_t ms = elapsedMs();
            const uint64_t nps = ms > 0 ? (nodes_ * 1000ull / static_cast<uint64_t>(ms)) : nodes_;
            std::cout << "info depth " << depth
                      << " score " << scoreToUci(score)
                      << " nodes " << nodes_
                      << " nps "   << nps
                      << " time "  << ms
                      << " pv "    << uci::moveToUci(rootBestCompleted_)
                      << std::endl;
        }

        // Soft stop: if over half the time budget is gone, don't open a new depth.
        if (useTime_ && elapsedMs() * 2 >= allocatedMs_) break;
        if (is_mate(score)) break;   // forced mate found
    }

    if (printBest)
        std::cout << "bestmove " << uci::moveToUci(rootBestCompleted_) << std::endl;
    return rootBestCompleted_;
}

} // namespace engine
