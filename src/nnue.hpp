#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include "types.hpp"

// NNUE evaluation (Phase 2, PLAN.md §19): a (768 -> HIDDEN)x2 SCReLU -> 1 network trained
// by bullet. Two accumulators (one per perspective) are kept up to date incrementally on
// make/unmake; evaluating reads them through the output layer. Behavior contract and the
// net-file layout in nnue.hpp.md.
namespace engine {

using chess::Color;

namespace nnue {

constexpr int INPUTS = 768; // colour (own/their) x 6 piece types x 64 squares
constexpr int HIDDEN = 512; // accumulator width per perspective
constexpr int QA     = 255; // feature-layer quantisation (also the SCReLU clamp)
constexpr int QB     = 64;  // output-layer quantisation
constexpr int SCALE  = 400; // network output units -> centipawns

// The quantised network exactly as bullet writes it: feature weights (one HIDDEN-wide
// column per input), feature biases, output weights (own perspective, then theirs),
// output bias, all little-endian int16. Every block is a multiple of 64 bytes, so the
// alignment adds no padding and the struct mirrors the file byte for byte.
struct Network {
    alignas(64) std::array<std::array<int16_t, HIDDEN>, INPUTS> featureWeights;
    alignas(64) std::array<int16_t, HIDDEN> featureBias;
    alignas(64) std::array<int16_t, 2 * HIDDEN> outputWeights;
    int16_t outputBias;
};

// The hidden layer of one position, from both perspectives (indexed by chess::Color).
struct Accumulator {
    alignas(64) std::array<std::array<int16_t, HIDDEN>, 2> values;
};

// Load a bullet `quantised.bin`; nullptr when the file is missing or the wrong size.
std::unique_ptr<Network> loadNetwork(const std::string& path);

// Build `out` from scratch for `board`.
void refresh(const Network& net, const Board& board, Accumulator& out);

// `child` = `parent` updated for `move`, which is about to be made on `board`.
void update(const Network& net, const Board& board, Move move, const Accumulator& parent,
            Accumulator& child);

// Side-to-move-relative evaluation (cp) of the position `acc` describes.
Value evaluate(const Network& net, const Accumulator& acc, Color stm);

} // namespace nnue
} // namespace engine
