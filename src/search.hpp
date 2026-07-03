#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include "types.hpp"

namespace engine {

class TranspositionTable;

// Everything the UCI `go` command can specify.
struct Limits {
    int     depth     = 0;   // fixed depth (0 = no depth cap)
    int64_t movetime  = 0;   // exact ms for this move (0 = none)
    int64_t nodes     = 0;   // node cap (0 = none)
    int64_t wtime = 0, btime = 0, winc = 0, binc = 0; // ms on the clocks
    int     movestogo = 0;
    bool    infinite  = false;
};

// Tunable time-management parameters (Phase 1a step 2). Scales are in permille
// (parts per 1000) of the base per-move slice, so they can be exposed as integer
// UCI spin options and A/B-tuned by self-play without a rebuild:
//   soft = base * softPermille / 1000   (don't *start* a new ID iteration past it)
//   hard = base * hardPermille / 1000   (abort the search mid-iteration)
// `assumedMovestogo` is the horizon used when the GUI sends sudden death (no
// `movestogo`); an explicit `movestogo` always wins.
struct TimeConfig {
    int softPermille     = 600;    // normal stop ~= 0.6 of the base slice
    int hardPermille     = 2400;   // safety abort ~= 4x the soft limit
    int assumedMovestogo = 30;
};

// A single search worker. Iterative deepening over a fail-soft (full-window)
// alpha-beta negamax with a transposition table and soft/hard time management.
// One instance per search; the shared `stop` flag lets the UCI thread abort it,
// and the transposition table is shared (and outlives the searcher) so results
// persist across moves and, later, across threads.
class Searcher {
public:
    Searcher(std::atomic<bool>& stopFlag, TranspositionTable& tt, TimeConfig tc = {})
        : stop_(stopFlag), tt_(tt), tc_(tc) {}

    // Runs the search on a copy of `board`. Prints `info` lines when
    // `printInfo`, and a final `bestmove` line when `printBest`.
    // Returns the best move found.
    Move think(Board board, const Limits& limits,
               bool printBest = true, bool printInfo = true);

    uint64_t nodes() const { return nodes_; }

private:
    Value   search(Board& board, int depth, Value alpha, Value beta, int ply);
    void    setupTiming(const Limits& limits, const Board& board);
    bool    checkStop();
    int64_t elapsedMs() const;

    std::atomic<bool>& stop_;
    TranspositionTable& tt_;
    TimeConfig tc_;
    uint64_t nodes_ = 0;

    std::chrono::steady_clock::time_point start_{};
    int64_t softLimitMs_ = 0;   // don't open a new depth past this (INT64_MAX if none)
    int64_t hardLimitMs_ = 0;   // abort the search past this (INT64_MAX if none)
    int64_t nodeLimit_   = 0;   // 0 = none
    bool    useTime_     = false;
    bool    timeUp_      = false;

    Move rootBest_          = Move(Move::NO_MOVE); // best of the in-progress iteration
    Move rootBestCompleted_ = Move(Move::NO_MOVE); // best of the last completed iteration
};

} // namespace engine
