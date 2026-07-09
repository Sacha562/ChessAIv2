#pragma once
#include <cstdint>
#include "chess.hpp"
#include "types.hpp"

//
// Quiet-move ordering heuristics (Phase 1b step 1). Beta-cutoffs teach the search
// which quiet moves tend to refute a position; recording that history lets later
// nodes try those moves earlier, which is the hard prerequisite for the pruning
// layers (NMP / LMR / LMP) that all assume the best move is searched first.
//
// One `History` bundles the three cutoff-driven signals:
//   - butterfly history: a saturating per-(color, from, to) score of cutoff success,
//   - killer moves: the two most recent quiet cutoff moves at each ply,
//   - countermoves: the quiet refutation indexed by the opponent's previous move.
//
// A `History` is owned per `Searcher` (per search), so it is single-thread state
// today and per-thread state under Lazy SMP later (Phase 1d) — no sharing, no locks.
//
namespace engine {

using chess::Color;

// Saturating bound on a butterfly-history entry: an entry stays inside
// [-MAX_HISTORY, MAX_HISTORY] under the gravity update, so move ordering can space
// its score buckets around this band. Public so `movepick` can static-assert its
// bucket spacing against it.
constexpr int MAX_HISTORY = 16384;

// Cutoff-driven move-ordering memory: butterfly history + killers + countermoves.
// Zero/NO_MOVE on construction, so a fresh search starts clean.
class History {
public:
    History() { clear(); }

    // Reset every table to empty (all-zero history, all-NO_MOVE killers/counters).
    void clear();

    // Enable/disable each signal independently. A disabled signal returns "empty" from
    // its query (so it has no ordering effect) while still being updated cheaply. Used
    // to A/B-isolate each heuristic's Elo via UCI toggles; all default on.
    void setEnabled(bool killers, bool history, bool countermove) {
        useKillers_ = killers;
        useHistory_ = history;
        useCounter_ = countermove;
    }

    // --- Queries (hot path: read once per move during ordering) ---

    // Butterfly-history score of quiet `move` for side `stm`, in
    // [-MAX_HISTORY, MAX_HISTORY]. Higher means "cut more often here before".
    [[nodiscard]] int quietScore(Color stm, Move move) const;

    // The killer move stored in `slot` (0 = most recent, 1 = older) at `ply`, or
    // NO_MOVE if none. `ply` must be in [0, MAX_PLY).
    [[nodiscard]] Move killer(int ply, int slot) const;

    // The stored countermove refuting `prevMove` (the move that produced the current
    // position), or NO_MOVE when there is no usable previous move. `board` is the
    // position *after* `prevMove`, so the moved piece sits on `prevMove.to()`.
    [[nodiscard]] Move counter(const Board& board, Move prevMove) const;

    // --- Update (cold-ish: once per quiet-move beta-cutoff) ---

    // Record a quiet-move fail-high at `ply`, remaining `depth`, for side `stm`:
    // reward `cutoff` in the butterfly table, penalise every quiet in
    // `[quietsTried, quietsTried + nTried)` that was searched first and failed to
    // cut, push `cutoff` into the killer slots, and store it as the countermove to
    // `prevMove`. `board` is the current node position (after `prevMove`).
    void updateQuietCutoff(const Board& board, Color stm, int ply, int depth, Move prevMove,
                           Move cutoff, const Move* quietsTried, int nTried);

private:
    static constexpr int NUM_COLORS  = 2;
    static constexpr int NUM_SQUARES = 64;
    static constexpr int NUM_PIECES  = 12; // piece-with-colour (WHITEPAWN..BLACKKING)
    static constexpr int NUM_KILLERS = 2;

    // Flat countermove slot for the move that reached this position, or -1 when there
    // is no usable previous move (no prev, or no piece on its target square).
    static int counterIndex(const Board& board, Move prevMove);

    int16_t butterfly_[NUM_COLORS][NUM_SQUARES][NUM_SQUARES];
    Move    killers_[MAX_PLY][NUM_KILLERS];
    Move    counters_[NUM_PIECES * NUM_SQUARES];

    bool useKillers_ = true; // per-signal on/off for A/B isolation (see setEnabled)
    bool useHistory_ = true;
    bool useCounter_ = true;
};

} // namespace engine
