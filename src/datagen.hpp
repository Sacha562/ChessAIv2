#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "types.hpp"
#include "viriformat.hpp"

// Self-play training-data generation for the Phase 2 NNUE (PLAN.md §20): independent
// per-thread workers play node-limited HCE games from random openings and append them
// to per-thread viriformat files. Behavior contract in datagen.hpp.md.
namespace engine {

struct DatagenConfig {
    std::string outPrefix;        // worker i appends to "<outPrefix>-<i>.vf"
    int         threads   = 1;    // independent game workers (>= 1)
    uint64_t    positions = 0;    // stop once this many positions are written in total
    uint64_t    seed      = 0;    // base RNG seed; worker i plays from a seed derived from it
    int64_t     softNodes = 5000; // per-move soft node budget (see Limits::softNodes)
};

// Adjudication of decided and dead games from the white-relative search score: a win
// once one side holds a winning score for several consecutive plies, a draw once a
// late-game score stays near zero for long enough.
class Adjudicator {
public:
    static constexpr Value WIN_SCORE    = 2500; // |score| (cp) that counts as decided
    static constexpr int   WIN_PLIES    = 4;    // consecutive decided plies to call a win
    static constexpr int   DRAW_MIN_PLY = 80;   // no draw adjudication before this game ply
    static constexpr Value DRAW_SCORE   = 10;   // |score| (cp) that counts as dead level
    static constexpr int   DRAW_PLIES   = 10;   // consecutive level plies to call a draw

    // Feed the score of the position at game ply `ply`; returns the outcome once the
    // game can be called, nothing while it must play on.
    std::optional<viriformat::Outcome> update(Value whiteScore, int ply);

private:
    int whiteWinPlies_ = 0;
    int blackWinPlies_ = 0;
    int drawPlies_     = 0;
};

// Run the generator until `config.positions` positions are written; blocks, printing a
// progress line periodically. Returns the process exit code (1 if an output file can't
// be opened).
int runDatagen(const DatagenConfig& config);

} // namespace engine
