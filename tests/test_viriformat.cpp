// Unit tests for the viriformat encoder (src/viriformat.*). Strategy: known-answer byte
// checks against the marlinformat / viriformat layout that bullet's ViriBinpackLoader
// decodes — the packed start position (occupancy, piece nibbles, unmoved-rook castling
// code, side-to-move / en-passant byte, clocks, WDL), the 16-bit move encoding for every
// move type, and the serialised game framing (header, little-endian entries, terminator).
#include "doctest.h"

#include <cstdint>
#include <sstream>
#include <string>

#include "chess.hpp"
#include "viriformat.hpp"

using namespace engine;
using namespace chess;

namespace {
using viriformat::Outcome;

uint16_t moveBits(const char* fen, const char* uci) {
    const Board board(fen);
    return viriformat::encodeMove(uci::uciToMove(board, uci));
}

uint64_t occupancy(const viriformat::PackedBoard& packed) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(packed[i]) << (8 * i);
    return v;
}

int nibble(const viriformat::PackedBoard& packed, int index) {
    return (packed[8 + index / 2] >> (4 * (index % 2))) & 0xF;
}

const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
} // namespace

TEST_CASE("startpos packs to the reference marlinformat bytes") {
    const viriformat::PackedBoard packed =
        viriformat::packBoard(Board(START_FEN), Outcome::WhiteWin);
    CHECK(occupancy(packed) == 0xFFFF00000000FFFFull);

    // White back rank R N B Q K B N R with both rooks still castling (code 6).
    const int whiteBack[] = {6, 1, 2, 4, 5, 2, 1, 6};
    for (int i = 0; i < 8; ++i)
        CHECK(nibble(packed, i) == whiteBack[i]);
    for (int i = 8; i < 16; ++i)
        CHECK(nibble(packed, i) == 0); // white pawns
    for (int i = 16; i < 24; ++i)
        CHECK(nibble(packed, i) == 8); // black pawns (colour bit 3)
    const int blackBack[] = {14, 9, 10, 12, 13, 10, 9, 14};
    for (int i = 0; i < 8; ++i)
        CHECK(nibble(packed, 24 + i) == blackBack[i]);

    CHECK(packed[24] == 64); // white to move, no en-passant square
    CHECK(packed[25] == 0);  // fifty-move clock
    CHECK(packed[26] == 1);  // fullmove number (u16 LE)
    CHECK(packed[27] == 0);
    CHECK(packed[28] == 0); // eval
    CHECK(packed[29] == 0);
    CHECK(packed[30] == 2); // WhiteWin
    CHECK(packed[31] == 0); // extra
}

TEST_CASE("only rooks that still carry a castling right get the unmoved-rook code") {
    // White may castle kingside only: h1 is code 6, a1 a plain rook (3); Black has none.
    const auto packed =
        viriformat::packBoard(Board("r3k2r/8/8/8/8/8/8/R3K2R w K - 0 1"), Outcome::Draw);
    CHECK(nibble(packed, 0) == 3);  // a1
    CHECK(nibble(packed, 1) == 5);  // e1
    CHECK(nibble(packed, 2) == 6);  // h1
    CHECK(nibble(packed, 3) == 11); // a8: black rook, no right
    CHECK(nibble(packed, 5) == 11); // h8
    CHECK(packed[30] == 1);         // Draw
}

TEST_CASE("side to move, en passant, and clocks land in their bytes") {
    const auto ep = viriformat::packBoard(
        Board("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3"), Outcome::Draw);
    CHECK(ep[24] == 45); // f6, white to move

    const auto black =
        viriformat::packBoard(Board("4k3/8/8/8/8/8/8/4K3 b - - 37 60"), Outcome::BlackWin);
    CHECK(black[24] == (1 << 7 | 64));
    CHECK(black[25] == 37);
    CHECK(black[26] == 60);
    CHECK(black[30] == 0); // BlackWin
}

TEST_CASE("moves encode as from | to << 6 | promo << 12 | flag << 14") {
    CHECK(moveBits(START_FEN, "e2e4") == (12 | 28 << 6));
    // Castling is "king takes own rook" in both libraries: e1 -> h1, castle flag 0b10.
    CHECK(moveBits("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1") == (4 | 7 << 6 | 0b10 << 14));
    CHECK(moveBits("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "e8c8") == (60 | 56 << 6 | 0b10 << 14));
    CHECK(moveBits("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3", "e5f6") ==
          (36 | 45 << 6 | 0b01 << 14));
    CHECK(moveBits("8/P7/8/8/8/8/8/k6K w - - 0 1", "a7a8q") ==
          (48 | 56 << 6 | 3 << 12 | 0b11 << 14));
    CHECK(moveBits("8/P7/8/8/8/8/8/k6K w - - 0 1", "a7a8n") ==
          (48 | 56 << 6 | 0 << 12 | 0b11 << 14));
}

TEST_CASE("a game serialises as header, LE entries, and a zero terminator") {
    const Board      start(START_FEN);
    viriformat::Game game;
    game.reset(start);
    game.addMove(uci::uciToMove(start, "e2e4"), -150);
    game.addMove(uci::uciToMove(start, "g1f3"), 40000); // clamped to int16
    CHECK(game.positions() == 2);

    std::ostringstream out;
    game.write(out, Outcome::BlackWin);
    const std::string bytes = out.str();
    REQUIRE(bytes.size() == 32 + 2 * 4 + 4);

    CHECK(static_cast<uint8_t>(bytes[30]) == 0); // outcome stamped into the header
    const auto u16 = [&](std::size_t at) {
        return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at]) |
                                     static_cast<uint8_t>(bytes[at + 1]) << 8);
    };
    CHECK(u16(32) == (12 | 28 << 6));
    CHECK(static_cast<int16_t>(u16(34)) == -150);
    CHECK(static_cast<int16_t>(u16(38)) == 32767);
    for (std::size_t i = 40; i < 44; ++i)
        CHECK(bytes[i] == 0);

    game.reset(start); // reset drops the entries but keeps a valid header
    CHECK(game.positions() == 0);
}
