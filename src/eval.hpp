#pragma once
#include "types.hpp"

namespace engine {

// Static evaluation from the side-to-move's perspective (centipawns).
// Phase 0: material only. PSQT / tapered / pawns / king-safety arrive in Phase 1c.
Value evaluate(const Board& board);

} // namespace engine
