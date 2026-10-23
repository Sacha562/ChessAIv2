#include "viriformat.hpp"
#include "chess.hpp"

#include <algorithm>
#include <ostream>

using namespace chess;

namespace engine {
namespace viriformat {

namespace {
// Marlinformat byte offsets inside the 32-byte packed board.
constexpr std::size_t OCCUPANCY_OFFSET = 0;  // u64 occupancy bitboard
constexpr std::size_t PIECES_OFFSET    = 8;  // 32 nibbles, one per occupied square
constexpr std::size_t STM_EP_OFFSET    = 24; // side to move << 7 | en-passant square
constexpr std::size_t HALFMOVE_OFFSET  = 25; // fifty-move clock
constexpr std::size_t FULLMOVE_OFFSET  = 26; // u16 fullmove number
constexpr std::size_t WDL_OFFSET       = 30; // Outcome; eval (28) and extra (31) stay 0

constexpr uint8_t  UNMOVED_ROOK = 6;  // piece code of a rook that still carries castling
constexpr uint8_t  COLOR_SHIFT  = 3;  // nibble = piece code | colour << 3
constexpr uint8_t  STM_SHIFT    = 7;  // byte 24 = stm << 7 | ep square
constexpr uint8_t  NO_EP_SQUARE = 64; // ep "square" when no en-passant capture exists
constexpr uint32_t MAX_HALFMOVE = 255;

constexpr int      TO_SHIFT         = 6;
constexpr int      PROMO_SHIFT      = 12;
constexpr uint16_t FLAG_EP          = 0b01 << 14;
constexpr uint16_t FLAG_CASTLE      = 0b10 << 14;
constexpr uint16_t FLAG_PROMO       = 0b11 << 14;
constexpr int      TERMINATOR_BYTES = 4; // a zero (move, score) entry ends the game

constexpr int16_t SCORE_MAX = 32767;

void putU16(PackedBoard& out, std::size_t offset, uint16_t v) {
    out[offset]     = static_cast<uint8_t>(v & 0xFF);
    out[offset + 1] = static_cast<uint8_t>(v >> 8);
}

void putU64(PackedBoard& out, std::size_t offset, uint64_t v) {
    for (std::size_t i = 0; i < sizeof(v); ++i)
        out[offset + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

void writeU16(std::ostream& out, uint16_t v) {
    const char bytes[] = {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
    out.write(bytes, sizeof(bytes));
}

// A back-rank rook whose file is still a castling right is packed as UNMOVED_ROOK —
// that is how marlinformat stores castling rights (it has no separate field). The
// side is the one the rook stands on relative to its own king.
bool isCastlingRook(const Board& board, Square sq, Color color) {
    if (sq.rank() != Rank::rank(Rank::RANK_1, color)) return false;
    const auto side   = board.kingSq(color).index() < sq.index()
                            ? Board::CastlingRights::Side::KING_SIDE
                            : Board::CastlingRights::Side::QUEEN_SIDE;
    const auto rights = board.castlingRights();
    return rights.has(color, side) && rights.getRookFile(color, side) == sq.file();
}

uint8_t pieceCode(const Board& board, Square sq) {
    const Piece     piece = board.at(sq);
    const Color     color = piece.color();
    const PieceType type  = piece.type();
    const bool      rook  = type == PieceType::ROOK && isCastlingRook(board, sq, color);
    const uint8_t   code  = rook ? UNMOVED_ROOK : static_cast<uint8_t>(static_cast<int>(type));
    return static_cast<uint8_t>(code | (static_cast<int>(color) << COLOR_SHIFT));
}
} // namespace

PackedBoard packBoard(const Board& board, Outcome outcome) {
    PackedBoard out{};
    Bitboard    occupied = board.occ();
    putU64(out, OCCUPANCY_OFFSET, occupied.getBits());

    // Nibble i describes the i-th occupied square in ascending (A1-first) order; even
    // indices take the low half of their byte.
    for (std::size_t i = 0; occupied; ++i) {
        const Square sq = Square(static_cast<int>(occupied.pop()));
        out[PIECES_OFFSET + i / 2] |= static_cast<uint8_t>(pieceCode(board, sq) << (4 * (i % 2)));
    }

    const Square  ep     = board.enpassantSq();
    const uint8_t epq    = ep == Square::NO_SQ ? NO_EP_SQUARE : static_cast<uint8_t>(ep.index());
    const uint8_t stm    = board.sideToMove() == Color::BLACK ? 1 : 0;
    out[STM_EP_OFFSET]   = static_cast<uint8_t>(stm << STM_SHIFT | epq);
    out[HALFMOVE_OFFSET] = static_cast<uint8_t>(std::min(board.halfMoveClock(), MAX_HALFMOVE));
    putU16(out, FULLMOVE_OFFSET, static_cast<uint16_t>(board.fullMoveNumber()));
    out[WDL_OFFSET] = static_cast<uint8_t>(outcome);
    return out;
}

uint16_t encodeMove(Move move) {
    const auto from = static_cast<uint16_t>(move.from().index());
    const auto to   = static_cast<uint16_t>(move.to().index() << TO_SHIFT);

    // Both libraries encode castling as "king takes own rook", so only the flag differs.
    switch (move.typeOf()) {
        case Move::ENPASSANT:
            return from | to | FLAG_EP;
        case Move::CASTLING:
            return from | to | FLAG_CASTLE;
        case Move::PROMOTION: {
            // Viriformat numbers promotions from the knight: N=0, B=1, R=2, Q=3.
            const int promo =
                static_cast<int>(move.promotionType()) - static_cast<int>(PieceType::KNIGHT);
            return static_cast<uint16_t>(from | to | promo << PROMO_SHIFT | FLAG_PROMO);
        }
        default:
            return from | to;
    }
}

void Game::reset(const Board& start) {
    start_ = packBoard(start, Outcome::Draw);
    entries_.clear();
}

void Game::addMove(Move move, Value whiteScore) {
    const auto score = static_cast<int16_t>(std::clamp<Value>(whiteScore, -SCORE_MAX, SCORE_MAX));
    entries_.push_back({encodeMove(move), score});
}

void Game::write(std::ostream& out, Outcome outcome) const {
    PackedBoard header = start_;
    header[WDL_OFFSET] = static_cast<uint8_t>(outcome);
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    for (const Entry& e : entries_) {
        writeU16(out, e.move);
        writeU16(out, static_cast<uint16_t>(e.score));
    }
    const char terminator[TERMINATOR_BYTES] = {};
    out.write(terminator, sizeof(terminator));
}

} // namespace viriformat
} // namespace engine
