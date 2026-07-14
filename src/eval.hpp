#pragma once
#include <array>
#include "types.hpp"

namespace engine {

// Tunable evaluation weights: combined piece-square tables with material folded in,
// one midgame (`mg`) and one endgame (`eg`) centipawn value per
// (piece type 0..5 = P,N,B,R,Q,K; square 0..63). Tables are stored in White's
// a8-first orientation (index 0 = a8, 63 = h1); `evaluate` flips the read for White
// pieces and mirrors it for Black. The search reads DEFAULT_EVAL_PARAMS; the Texel
// tuner mutates a copy of it and writes the optimized tables back as the new seed.
struct EvalParams {
    std::array<std::array<int16_t, 64>, 6> mg{};
    std::array<std::array<int16_t, 64>, 6> eg{};

    // Mobility: midgame/endgame bonus indexed by the number of *safe* target squares
    // a piece attacks (squares not occupied by a friendly piece and not attacked by an
    // enemy pawn). One table per mobile piece, index 0=knight, 1=bishop, 2=rook,
    // 3=queen; 28 slots covers the queen's maximum. Pawns and the king have no
    // mobility term.
    std::array<std::array<int16_t, 28>, 4> mobMg{};
    std::array<std::array<int16_t, 28>, 4> mobEg{};

    // Pawn structure. Passed-pawn bonus is indexed by the pawn's rank relative to its
    // own side (0 = own back rank, 7 = promotion; both ends unused). Isolated and
    // doubled are penalties (<= 0): isolated = no friendly pawn on an adjacent file;
    // doubled applies per extra pawn sharing a file.
    std::array<int16_t, 8> passedMg{};
    std::array<int16_t, 8> passedEg{};
    int16_t                isolatedMg{};
    int16_t                isolatedEg{};
    int16_t                doubledMg{};
    int16_t                doubledEg{};

    // Piece terms. Bishop pair: holding both bishops. Rook file: open = no pawns of
    // either colour on the file, semi = no friendly pawns (enemy pawns present).
    // Rook 7th: rook on its relative 7th rank. Knight outpost: a knight on the
    // relative 4th-6th rank, defended by a friendly pawn, that no enemy pawn can
    // advance to attack.
    int16_t bishopPairMg{}, bishopPairEg{};
    int16_t rookOpenMg{}, rookOpenEg{};
    int16_t rookSemiMg{}, rookSemiEg{};
    int16_t rookSeventhMg{}, rookSeventhEg{};
    int16_t knightOutpostMg{}, knightOutpostEg{};
};

// PeSTO/Kaufman-seeded defaults, baked at namespace scope (see eval.cpp). The search
// hot path evaluates against these; they are the starting point for Texel tuning.
extern const EvalParams DEFAULT_EVAL_PARAMS;

// Static evaluation from the side-to-move's perspective (centipawns), tapered
// midgame <-> endgame by remaining non-pawn material (phase 24 = opening, 0 = bare
// endgame). `tempo` is the side-to-move bonus (a self-play-tunable knob; see
// engine::Tunables). Positive favors the side to move.
//
// The 2-arg form is the search hot path (baked DEFAULT_EVAL_PARAMS). The 3-arg form
// scores against a candidate parameter set and is used by the Texel tuner.
Value evaluate(const Board& board, Value tempo);
Value evaluate(const Board& board, const EvalParams& params, Value tempo);

} // namespace engine
