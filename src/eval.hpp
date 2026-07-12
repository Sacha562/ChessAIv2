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
