#pragma once
#include <cstdint>
#include <memory>
#include "chess.hpp"
#include "types.hpp"

//
// Quiet-move ordering heuristics (Phase 1b step 1; continuation history in Phase 1c).
// Beta-cutoffs teach the search which quiet moves tend to refute a position; recording
// that history lets later nodes try those moves earlier, which is the hard prerequisite
// for the pruning layers (NMP / LMR / LMP) that all assume the best move is searched
// first.
//
// One `History` bundles the cutoff-driven signals:
//   - butterfly history: a saturating per-(color, from, to) score of cutoff success,
//   - killer moves: the two most recent quiet cutoff moves at each ply,
//   - countermoves: the quiet refutation indexed by the opponent's previous move,
//   - continuation history: a saturating score for the current move conditioned on the
//     piece+to of a recent move (1 or 2 plies back) — a scored generalisation of the
//     countermove, among the strongest modern quiet-ordering signals.
//
// A `History` is owned per `Searcher` (per search), so it is single-thread state
// today and per-thread state under Lazy SMP later (Phase 1d) — no sharing, no locks.
//
namespace engine {

using chess::Color;
using chess::Piece;
using chess::Square;

// Saturating bound on a butterfly-history entry: an entry stays inside
// [-MAX_HISTORY, MAX_HISTORY] under the gravity update, so move ordering can space
// its score buckets around this band. Public so `movepick` can static-assert its
// bucket spacing against it.
constexpr int MAX_HISTORY = 16384;

// Number of prior plies continuation history conditions on (the moves 1 and 2 plies
// back). Public so movepick can size its quiet-score band against it.
constexpr int CONT_PLIES = 2;

// Resolved recent-move context for continuation history: the (piece, to) of the moves
// played 1 and 2 plies before the current node. A pair is absent (piece < 0) at the
// root, after a null move, or before enough plies exist. Built by [`search`] and passed
// to both the ordering query ([`continuationScore`]) and the cutoff update
// ([`updateQuietCutoff`]). `piece` is a colour+type index (0–11); `to` a square (0–63).
struct ContHistContext {
    int piece1 = -1, to1 = -1; // move one ply ago
    int piece2 = -1, to2 = -1; // move two plies ago
};

// Cutoff-driven move-ordering memory: butterfly history + killers + countermoves +
// continuation history. Zero/NO_MOVE on construction, so a fresh search starts clean.
class History {
public:
    History() : continuation_(std::make_unique<ContTable>()) { clear(); }

    // Reset every table to empty (all-zero history, all-NO_MOVE killers/counters).
    void clear();

    // Enable/disable each signal independently. A disabled signal returns "empty" from
    // its query (so it has no ordering effect) while still being updated cheaply. Used
    // to A/B-isolate each heuristic's Elo via UCI toggles; all default on.
    void setEnabled(bool killers, bool history, bool countermove, bool contHist) {
        useKillers_  = killers;
        useHistory_  = history;
        useCounter_  = countermove;
        useContHist_ = contHist;
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

    // Continuation-history score for playing (`movedPiece`, `to`) given the recent-move
    // context `ctx`, summed over the available prior plies. In [-CONT_PLIES*MAX_HISTORY,
    // CONT_PLIES*MAX_HISTORY]. Returns 0 when continuation history is disabled or `ctx`
    // has no prior move. `movedPiece` is a colour+type index (0–11); `to` a square (0–63).
    [[nodiscard]] int continuationScore(const ContHistContext& ctx, int movedPiece, int to) const;

    // --- Update (cold-ish: once per quiet-move beta-cutoff) ---

    // Record a quiet-move fail-high at `ply`, remaining `depth`, for side `stm`:
    // reward `cutoff` in the butterfly and continuation tables, penalise every quiet in
    // `[quietsTried, quietsTried + nTried)` that was searched first and failed to cut,
    // push `cutoff` into the killer slots, and store it as the countermove to
    // `prevMove`. `board` is the current node position (after `prevMove`); `ctx` is the
    // continuation context (recent moves) used to key the continuation update.
    void updateQuietCutoff(const Board& board, Color stm, int ply, int depth, Move prevMove,
                           Move cutoff, const Move* quietsTried, int nTried,
                           const ContHistContext& ctx);

private:
    static constexpr int NUM_COLORS  = 2;
    static constexpr int NUM_SQUARES = 64;
    static constexpr int NUM_PIECES  = 12; // piece-with-colour (WHITEPAWN..BLACKKING)
    static constexpr int NUM_KILLERS = 2;

    // Flat countermove slot for the move that reached this position, or -1 when there
    // is no usable previous move (no prev, or no piece on its target square).
    static int counterIndex(const Board& board, Move prevMove);

    // Continuation-history table: a saturating score indexed by the prior move's
    // (piece, to) and the current move's (piece, to). ~1.13 MB, so it is heap-owned via
    // `continuation_` — the `Searcher` (and thus `History`) lives on the search thread's
    // stack, which is too small for it as a direct member.
    struct ContTable {
        int16_t v[NUM_PIECES][NUM_SQUARES][NUM_PIECES][NUM_SQUARES];
    };

    // Apply `bonus` (via the gravity update) to the continuation entry for playing
    // (`movedPiece`, `to`) under each prior ply present in `ctx`.
    void updateContinuation(const ContHistContext& ctx, int movedPiece, int to, int bonus);

    int16_t                    butterfly_[NUM_COLORS][NUM_SQUARES][NUM_SQUARES];
    Move                       killers_[MAX_PLY][NUM_KILLERS];
    Move                       counters_[NUM_PIECES * NUM_SQUARES];
    std::unique_ptr<ContTable> continuation_;

    bool useKillers_  = true; // per-signal on/off for A/B isolation (see setEnabled)
    bool useHistory_  = true;
    bool useCounter_  = true;
    bool useContHist_ = true;
};

} // namespace engine
