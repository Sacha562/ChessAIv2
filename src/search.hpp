#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include "history.hpp"
#include "types.hpp"

namespace engine {

class TranspositionTable;

// Everything the UCI `go` command can specify.
struct Limits {
    int     depth    = 0;                             // fixed depth (0 = no depth cap)
    int64_t movetime = 0;                             // exact ms for this move (0 = none)
    int64_t nodes    = 0;                             // node cap (0 = none)
    int64_t wtime = 0, btime = 0, winc = 0, binc = 0; // ms on the clocks
    int     movestogo = 0;
    bool    infinite  = false;
};

// Self-play-tunable engine knobs. Held per-search and copied into each Searcher, so
// they can be A/B-tuned by self-play (SPSA) through UCI spin options without a
// rebuild. Grouped by subsystem.
//
// Time management — scales are in permille (parts per 1000) of the base per-move
// slice `base = remaining/movestogo + inc/2`:
//   soft = base * softPermille / 1000   (don't *start* a new ID iteration past it)
//   hard = base * hardPermille / 1000   (abort the search mid-iteration)
// `assumedMovestogo` is the horizon used under sudden death (no `movestogo`); an
// explicit `movestogo` always wins. It divides the clock, so it must stay >= 1.
struct Tunables {
    // Time management.
    int softPermille     = 600;  // normal stop ~= 0.6 of the base slice
    int hardPermille     = 2400; // safety abort ~= 4x the soft limit
    int assumedMovestogo = 30;   // assumed moves-to-go under sudden death (>= 1)

    // Evaluation.
    Value tempo = 9; // side-to-move bonus (centipawns)

    // Quiescence delta pruning.
    Value deltaMargin   = 203; // delta-pruning safety cushion (centipawns)
    int   endgamePieces = 7;   // total pieces at/below which delta pruning is off

    // Ordering-heuristic on/off toggles, all defaulted from A/B self-play. Killers
    // (+44 Elo) and countermove (~0) are on. Butterfly history and IIR both *inverted*
    // once the pruning stack (LMP/LMR) existed to consume them: history went -23 -> +247
    // and IIR -36 -> +16, because LMP/LMR prune/reduce late-ordered quiets, so good
    // ordering / a hash-move-first search is what stops them discarding a winning move
    // (PLAN.md Part 4 — "ordering makes pruning safe"). All four are on and stay
    // UCI-tunable for A/B isolation.
    bool useKillers     = true;
    bool useHistory     = true;
    bool useCountermove = true;
    bool useIir         = true;

    // Phase 1b pruning / reduction / extension toggles (each individually ablatable).
    bool useNmp        = true; // null-move pruning
    bool useRfp        = true; // reverse futility (static null-move) pruning
    bool useFutility   = true; // futility pruning of quiets near the horizon
    bool useLmp        = true; // late-move (move-count) pruning
    bool useLmr        = true; // late move reductions
    bool useCheckExt   = true; // check extensions
    bool useAspiration = true; // aspiration windows at the root

    // Aspiration: initial half-window (cp) around the previous iteration's score.
    Value aspirationDelta = 15;
};

// A single search worker. Iterative deepening over a fail-soft Principal Variation
// Search (null-window scout + re-search) with a transposition table and soft/hard
// time management.
// One instance per search; the shared `stop` flag lets the UCI thread abort it,
// and the transposition table is shared (and outlives the searcher) so results
// persist across moves and, later, across threads.
class Searcher {
public:
    Searcher(std::atomic<bool>& stopFlag, TranspositionTable& tt, Tunables tp = {})
        : stop_(stopFlag), tt_(tt), tp_(tp) {}

    // Runs the search on a copy of `board`. Prints `info` lines when
    // `printInfo`, and a final `bestmove` line when `printBest`.
    // Returns the best move found.
    Move think(Board board, const Limits& limits, bool printBest = true, bool printInfo = true);

    uint64_t nodes() const { return nodes_; }

private:
    Value   search(Board& board, int depth, Value alpha, Value beta, int ply, Move prevMove);
    Value   aspirationSearch(Board& board, int depth, Value prevScore);
    Value   qsearch(Board& board, Value alpha, Value beta, int ply);
    void    setupTiming(const Limits& limits, const Board& board);
    bool    checkStop();
    int64_t elapsedMs() const;

    std::atomic<bool>&  stop_;
    TranspositionTable& tt_;
    Tunables            tp_;
    uint64_t            nodes_ = 0;
    History             history_;                // quiet-move ordering heuristics (per search)
    Value               staticEvals_[MAX_PLY]{}; // per-ply static eval, for RFP/futility/improving

    std::chrono::steady_clock::time_point start_{};
    int64_t softLimitMs_ = 0; // don't open a new depth past this (INT64_MAX if none)
    int64_t hardLimitMs_ = 0; // abort the search past this (INT64_MAX if none)
    int64_t nodeLimit_   = 0; // 0 = none
    bool    useTime_     = false;
    bool    timeUp_      = false;

    Move rootBest_          = Move(Move::NO_MOVE); // best of the in-progress iteration
    Move rootBestCompleted_ = Move(Move::NO_MOVE); // best of the last completed iteration
};

} // namespace engine
