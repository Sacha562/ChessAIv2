#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>
#include "types.hpp"

// Viriformat: the self-play game format bullet's ViriBinpackLoader reads (Phase 2 NNUE
// training data). A game is a 32-byte packed start position (marlinformat), then one
// 4-byte (move, white-relative score) entry per ply, then a 4-byte zero terminator.
// Every multi-byte field is little-endian. Full layout in viriformat.hpp.md.
namespace engine {
namespace viriformat {

constexpr std::size_t PACKED_BOARD_BYTES = 32;

using PackedBoard = std::array<uint8_t, PACKED_BOARD_BYTES>;

// Game result from White's point of view, with marlinformat's packed values.
enum class Outcome : uint8_t {
    BlackWin = 0,
    Draw     = 1,
    WhiteWin = 2
};

// Pack `board` into marlinformat with `outcome` as its WDL byte (eval and extra are 0).
PackedBoard packBoard(const Board& board, Outcome outcome);

// The 16-bit viriformat encoding of `move`: from | to << 6 | promo << 12 | flag << 14.
uint16_t encodeMove(Move move);

// One self-play game: the start position plus a (move, score) entry per ply played.
// `reset` reuses the entry buffer, so a worker can hold one Game for its whole run.
class Game {
public:
    void        reset(const Board& start);
    void        addMove(Move move, Value whiteScore);
    std::size_t positions() const { return entries_.size(); }

    // Append the serialised game, with `outcome` stamped into its header, to `out`.
    void write(std::ostream& out, Outcome outcome) const;

private:
    struct Entry {
        uint16_t move;
        int16_t  score;
    };

    PackedBoard        start_{}; // packed as a draw; the real outcome is stamped by write
    std::vector<Entry> entries_;
};

} // namespace viriformat
} // namespace engine
