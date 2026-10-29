#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "types.hpp"

// NNUE evaluation (Phase 2, PLAN.md §19): a (768 x INPUT_BUCKETS -> HIDDEN)x2 SCReLU ->
// OUTPUT_BUCKETS network trained by bullet. Each perspective's inputs are mirrored a<->h
// when its own king is on files e-h, and can be bucketed by that king's square (the
// current net uses a single bucket). Two accumulators (one per perspective) are kept up
// to date incrementally on make/unmake, rebuilt for a perspective whose king changes
// bucket or side; evaluating reads them through the output layer row picked by the
// position's piece count. Behavior contract and the net-file layout in nnue.hpp.md.
namespace engine {

using chess::Color;

namespace nnue {

constexpr int INPUTS         = 768;  // per bucket: colour (own/their) x 6 piece types x 64 squares
constexpr int HIDDEN         = 1024; // accumulator width per perspective
constexpr int OUTPUT_BUCKETS = 8;    // output layers, chosen by piece count

// Input bucket of a perspective's king, by its square seen from that side with files
// e-h mirrored onto d-a (4 files x 8 ranks, a1-d1 first). Must match the trainer's layout.
// A single bucket for now: 10 king buckets lost Elo on our ~400M positions (their
// per-square weights learned noise), so the net keeps only the a<->h mirroring.
constexpr std::array<int, 32> KING_BUCKET_LAYOUT = {};

constexpr int INPUT_BUCKETS = 1;   // distinct values in KING_BUCKET_LAYOUT
constexpr int QA            = 255; // feature-layer quantisation (also the SCReLU clamp)
constexpr int QB            = 64;  // output-layer quantisation
constexpr int SCALE         = 400; // network output units -> centipawns

// The quantised network exactly as bullet writes it: feature weights (one HIDDEN-wide
// column per input, bucket-major), feature biases, output weights (one row per bucket: own
// perspective, then theirs), output biases, all little-endian int16. Every block but the
// last is a multiple of 64 bytes, so the alignment adds no padding and the struct mirrors
// the file.
struct Network {
    alignas(64) std::array<std::array<int16_t, HIDDEN>, INPUT_BUCKETS * INPUTS> featureWeights;
    alignas(64) std::array<int16_t, HIDDEN> featureBias;
    alignas(64) std::array<std::array<int16_t, 2 * HIDDEN>, OUTPUT_BUCKETS> outputWeights;
    std::array<int16_t, OUTPUT_BUCKETS> outputBias;
};

// The hidden layer of one position, from both perspectives (indexed by chess::Color).
struct Accumulator {
    alignas(64) std::array<std::array<int16_t, HIDDEN>, 2> values;
};

// Load a bullet `quantised.bin`; nullptr when the file is missing or the wrong size.
std::unique_ptr<Network> loadNetwork(const std::string& path);

// Copy a network out of `size` bytes of `quantised.bin` contents; nullptr when too short.
std::unique_ptr<Network> networkFromBytes(const unsigned char* data, std::size_t size);

// The network linked into the binary at build time (the default eval), or nullptr when
// the build embedded none. Built on first call; valid for the life of the program.
const Network* embeddedNetwork();

// Build `out` from scratch for `board`.
void refresh(const Network& net, const Board& board, Accumulator& out);

// `child` = `parent` updated for `move`, which is about to be made on `board`.
void update(const Network& net, const Board& board, Move move, const Accumulator& parent,
            Accumulator& child);

// Side-to-move-relative evaluation (cp) of `board`, whose accumulator is `acc`.
Value evaluate(const Network& net, const Accumulator& acc, const Board& board);

} // namespace nnue
} // namespace engine
