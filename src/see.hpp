#pragma once
#include "chess.hpp"
#include "types.hpp"

//
// Static Exchange Evaluation (SEE).
//
// Given a move, SEE computes the net material a side wins or loses by initiating
// the capture sequence on the move's target square, assuming both sides keep
// recapturing with their least-valuable attacker and either may stop when ahead.
// It is a pure, read-only query — the board is never mutated.
//
// One primitive powers three consumers: the capture good/bad split in move
// ordering, quiescence capture pruning, and (later) LMR / check-extension gating.
//
namespace engine {

// Material scale used across the search side (SEE, MVV-LVA, delta pruning). It is
// intentionally independent of eval's tunable values — SEE wants simple, stable
// integers, not whatever the evaluation happens to be tuned to. Indexed by
// `PieceType` (PAWN..KING, then NONE). The king value is unused: the swap never
// assigns a value to a king capture (a king may only take when undefended).
inline constexpr Value SEE_PIECE_VALUE[] = {
    100,  // PAWN
    320,  // KNIGHT
    330,  // BISHOP
    500,  // ROOK
    900,  // QUEEN
    0,    // KING   (never scored as a captured/capturing value)
    0,    // NONE   (empty square / quiet move)
};

// Material value of `pt` on the search-side scale. `PieceType::NONE` maps to 0.
[[nodiscard]] Value pieceValue(chess::PieceType pt);

// True iff the static exchange evaluation of `move` on `board` is at least
// `threshold` centipawns (from the moving side's perspective). This is the common
// consumer query ("is this capture non-losing?" == `seeGE(board, move, 0)`); it is
// cheaper than computing the full signed value because it stops as soon as the sign
// relative to `threshold` is settled.
//
// Handles en passant, queen promotions (under-promotions are scored as queens),
// x-ray / battery attackers revealed behind a moved slider, and the rule that a
// king may only capture into a square the opponent no longer defends. Pins are not
// modelled (a pinned defender is still counted) — a small, standard approximation.
[[nodiscard]] bool seeGE(const Board& board, Move move, Value threshold);

} // namespace engine
