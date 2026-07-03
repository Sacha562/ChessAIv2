#pragma once
#include "types.hpp"

namespace engine {

// Static evaluation from the side-to-move's perspective (centipawns).
// `tempo` is the side-to-move bonus, a self-play-tunable knob supplied by the
// caller (see engine::Tunables). Phase 0: material only. PSQT / tapered / pawns /
// king-safety arrive in Phase 1c.
Value evaluate(const Board& board, Value tempo);

} // namespace engine
