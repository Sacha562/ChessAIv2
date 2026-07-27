#pragma once
#include <cstdint>
#include "chess.hpp"
#include "types.hpp"

//
// Correction history (Phase 1d): a learned per-pawn-structure correction to the static
// evaluation.
//
// The hand-crafted eval is a static guess; the search is the ground truth. Whenever a
// node finishes, the gap between the two (`searchScore - staticEval`) is a residual the
// eval got wrong in *this kind of position*. Correction history remembers that residual,
// bucketed by the pawn skeleton and the side to move, and adds it back the next time a
// similar position is evaluated — so reverse futility, futility, null-move pruning and
// the `improving` trend all reason about a static eval the search has already been seen
// to disagree with.
//
// Bucketing by the **pawn structure** is what makes this work: pawns move rarely, so the
// same bucket is revisited constantly within a search, and the pawn skeleton is exactly
// what a hand-crafted eval judges worst (long-term structural compensation). The key is a
// cheap multiply-mix of the two pawn bitboards, not a Zobrist key — a couple of ALU ops
// on the per-node path, and collisions only blur two buckets together.
//
// A `CorrectionHistory` is owned per `Searcher` (per search), so it is single-thread
// state today and per-thread state under Lazy SMP later — no sharing, no locks.
//
namespace engine {

using chess::Color;

// Fixed-point scale of a stored entry: an entry holds `centipawns * CORR_GRAIN`, so the
// running average keeps sub-centipawn resolution inside its int16_t slot.
constexpr int CORR_GRAIN = 256;

// Saturating bound on a stored entry — and therefore the largest correction ever applied
// to a static eval: +/-64 cp. Small on purpose; the correction nudges the eval, it does
// not overrule it.
constexpr int CORR_LIMIT = 64 * CORR_GRAIN;

// Cheap hash of the pawn skeleton (both colours' pawn bitboards) that buckets the table.
// Public so tests can assert the bucketing contract.
[[nodiscard]] uint64_t pawnStructureKey(const Board& board);

// Per-(side to move, pawn structure) running average of the search-vs-static-eval
// residual. Zeroed on construction, so a fresh search starts with an uncorrected eval.
class CorrectionHistory {
public:
    CorrectionHistory() { clear(); }

    // Reset every bucket to "no correction known".
    void clear();

    // The correction (cp) learned for `board`'s pawn structure and side to move, in
    // [-CORR_LIMIT / CORR_GRAIN, +CORR_LIMIT / CORR_GRAIN]. Hot path: one mix + one array
    // read.
    [[nodiscard]] Value correction(const Board& board) const;

    // `raw` (a side-to-move-relative static eval) plus that correction, clamped to stay
    // clear of the mate range so a corrected eval can never look like a forced mate.
    [[nodiscard]] Value corrected(const Board& board, Value raw) const;

    // Fold one node's residual `diff = searchScore - staticEval` (cp) into the bucket, as
    // a depth-weighted running average: a deeper node's disagreement is more trustworthy,
    // so it moves the entry further. Called once per completed non-check node whose best
    // move was quiet and whose bound agrees with the residual's sign.
    void update(const Board& board, int depth, Value diff);

private:
    static constexpr int SIZE       = 16384; // buckets per colour (power of two)
    static constexpr int MAX_WEIGHT = 16;    // depth at which a residual carries full weight
    static constexpr int WEIGHT_DIV = 2 * MAX_WEIGHT; // running-average denominator

    static int index(const Board& board);

    int16_t table_[2][SIZE];
};

} // namespace engine
