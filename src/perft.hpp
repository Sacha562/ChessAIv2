#pragma once
#include <cstdint>
#include "chess.hpp"

namespace engine {
namespace Perft {

// Count leaf nodes at the given depth.
uint64_t perft(chess::Board& board, int depth);

// Perft with a per-root-move breakdown (like Stockfish's `go perft`).
void divide(chess::Board& board, int depth);

// Run a suite of known positions; returns true iff every count matches.
bool test();

} // namespace Perft
} // namespace engine
