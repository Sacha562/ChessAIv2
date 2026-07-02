#pragma once
//
// Core engine types and score conventions.
//
// Scores are in centipawns from the side-to-move's perspective (negamax).
// Mate scores are encoded as `VALUE_MATE - ply`, so a shorter mate scores
// higher and mate scores survive being stored/retrieved (later) in the TT.
//
#include <cstdint>
#include "chess.hpp"

namespace engine {

using chess::Board;
using chess::Move;

using Value = int;

constexpr int MAX_PLY = 128;

constexpr Value VALUE_ZERO     = 0;
constexpr Value VALUE_DRAW     = 0;
constexpr Value VALUE_MATE     = 32000;
constexpr Value VALUE_INFINITE = 32001;
constexpr Value VALUE_NONE     = 32002;

constexpr Value VALUE_MATE_IN_MAX_PLY  = VALUE_MATE - MAX_PLY;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;

// A mate delivered `ply` plies from the root.
constexpr Value mate_in(int ply)  { return VALUE_MATE - ply; }
// Being mated `ply` plies from the root.
constexpr Value mated_in(int ply) { return -VALUE_MATE + ply; }

constexpr bool is_mate(Value v) {
    return v >= VALUE_MATE_IN_MAX_PLY || v <= VALUE_MATED_IN_MAX_PLY;
}

} // namespace engine
