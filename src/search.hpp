#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include "types.hpp"

namespace engine {

// Everything the UCI `go` command can specify.
struct Limits {
    int     depth     = 0;   // fixed depth (0 = no depth cap)
    int64_t movetime  = 0;   // exact ms for this move (0 = none)
    int64_t nodes     = 0;   // node cap (0 = none)
    int64_t wtime = 0, btime = 0, winc = 0, binc = 0; // ms on the clocks
    int     movestogo = 0;
    bool    infinite  = false;
};

// A single search worker. Phase 0: iterative deepening over a plain
// (full-window) alpha-beta negamax. One instance per search; a shared
// `stop` flag lets the UCI thread abort it.
class Searcher {
public:
    explicit Searcher(std::atomic<bool>& stopFlag) : stop_(stopFlag) {}

    // Runs the search on a copy of `board`. Prints `info` lines when
    // `printInfo`, and a final `bestmove` line when `printBest`.
    // Returns the best move found.
    Move think(Board board, const Limits& limits,
               bool printBest = true, bool printInfo = true);

    uint64_t nodes() const { return nodes_; }

private:
    Value   search(Board& board, int depth, Value alpha, Value beta, int ply);
    bool    checkStop();
    int64_t elapsedMs() const;

    std::atomic<bool>& stop_;
    uint64_t nodes_ = 0;

    std::chrono::steady_clock::time_point start_{};
    int64_t allocatedMs_ = 0;   // hard time budget (INT64_MAX if none)
    int64_t nodeLimit_   = 0;   // 0 = none
    bool    useTime_     = false;
    bool    timeUp_      = false;

    Move rootBest_          = Move(Move::NO_MOVE); // best of the in-progress iteration
    Move rootBestCompleted_ = Move(Move::NO_MOVE); // best of the last completed iteration
};

} // namespace engine
