#include "see.hpp"

#include "chess.hpp"

using namespace chess;

namespace engine {

namespace {

// All pieces (both colors) attacking `sq` under the hypothetical occupancy `occ`.
// Sliders are re-derived through `occ`, so as the swap loop peels attackers off the
// board this reveals the x-ray / battery attackers lined up behind them.
Bitboard attackersTo(const Board& board, Square sq, Bitboard occ) {
    const Bitboard bishops = board.pieces(PieceType::BISHOP) | board.pieces(PieceType::QUEEN);
    const Bitboard rooks   = board.pieces(PieceType::ROOK) | board.pieces(PieceType::QUEEN);

    // A `color` pawn attacks `sq` iff an enemy pawn placed on `sq` would attack it,
    // hence the swapped color on `attacks::pawn`.
    return (attacks::pawn(Color::BLACK, sq) & board.pieces(PieceType::PAWN, Color::WHITE))
         | (attacks::pawn(Color::WHITE, sq) & board.pieces(PieceType::PAWN, Color::BLACK))
         | (attacks::knight(sq) & board.pieces(PieceType::KNIGHT))
         | (attacks::king(sq) & board.pieces(PieceType::KING))
         | (attacks::bishop(sq, occ) & bishops)
         | (attacks::rook(sq, occ) & rooks);
}

Bitboard lsbBB(Bitboard bb) { return Bitboard::fromSquare(bb.lsb()); }

} // namespace

Value pieceValue(PieceType pt) {
    return SEE_PIECE_VALUE[static_cast<int>(pt.internal())];
}

// Balance-based SEE threshold test, after the Stockfish `see_ge` swap. `swap` holds
// the running exchange balance; `res` (0/1) tracks which side is on move so the
// final parity answers "does the initiating side keep >= threshold?".
bool seeGE(const Board& board, Move move, Value threshold) {
    // Castling moves nothing capturable.
    if (move.typeOf() == Move::CASTLING)
        return threshold <= 0;

    const Square from      = move.from();
    const Square to        = move.to();
    const bool   enPassant = move.typeOf() == Move::ENPASSANT;
    const bool   promotion = move.typeOf() == Move::PROMOTION;

    // Value the first move gains: the captured piece, plus the queening bonus when
    // the mover promotes. A quiet move captures nothing (value 0).
    Value swap = enPassant ? pieceValue(PieceType::PAWN) : pieceValue(board.at<PieceType>(to));
    if (promotion)
        swap += pieceValue(PieceType::QUEEN) - pieceValue(PieceType::PAWN);
    swap -= threshold;
    if (swap < 0)
        return false;   // even capturing for free falls short of the threshold

    // The piece now standing on `to`, exposed to recapture (a queen if we promoted).
    const Value moverValue = promotion ? pieceValue(PieceType::QUEEN)
                                       : pieceValue(board.at<PieceType>(from));
    swap = moverValue - swap;
    if (swap <= 0)
        return true;    // a single recapture cannot drag us below the threshold

    // Occupancy after the initial move: the mover has left `from`, and the captured
    // piece (on `to`, or the ep square for en passant) is gone. A quiet move leaves
    // `to` empty and untouched.
    Bitboard occ = board.occ() ^ Bitboard::fromSquare(from);
    if (enPassant)
        occ ^= Bitboard::fromSquare(to.ep_square());
    else if (board.isCapture(move))
        occ ^= Bitboard::fromSquare(to);

    const Bitboard bishops = board.pieces(PieceType::BISHOP) | board.pieces(PieceType::QUEEN);
    const Bitboard rooks   = board.pieces(PieceType::ROOK) | board.pieces(PieceType::QUEEN);

    Bitboard attackers = attackersTo(board, to, occ) & occ;
    Color    stm       = board.sideToMove();
    int      res       = 1;

    while (true) {
        stm       = ~stm;
        attackers &= occ;
        const Bitboard stmAttackers = attackers & board.us(stm);
        if (stmAttackers.empty())
            break;   // side to move cannot recapture: the balance stands

        res ^= 1;

        // Take with the least-valuable attacker; peel it off `occ` and rescan the
        // slider rays it may have been blocking (x-ray / battery reveal).
        Bitboard bb;
        if (bb = stmAttackers & board.pieces(PieceType::PAWN); !bb.empty()) {
            if ((swap = pieceValue(PieceType::PAWN) - swap) < res)
                break;
            occ ^= lsbBB(bb);
            attackers |= attacks::bishop(to, occ) & bishops;
        } else if (bb = stmAttackers & board.pieces(PieceType::KNIGHT); !bb.empty()) {
            if ((swap = pieceValue(PieceType::KNIGHT) - swap) < res)
                break;
            occ ^= lsbBB(bb);
        } else if (bb = stmAttackers & board.pieces(PieceType::BISHOP); !bb.empty()) {
            if ((swap = pieceValue(PieceType::BISHOP) - swap) < res)
                break;
            occ ^= lsbBB(bb);
            attackers |= attacks::bishop(to, occ) & bishops;
        } else if (bb = stmAttackers & board.pieces(PieceType::ROOK); !bb.empty()) {
            if ((swap = pieceValue(PieceType::ROOK) - swap) < res)
                break;
            occ ^= lsbBB(bb);
            attackers |= attacks::rook(to, occ) & rooks;
        } else if (bb = stmAttackers & board.pieces(PieceType::QUEEN); !bb.empty()) {
            if ((swap = pieceValue(PieceType::QUEEN) - swap) < res)
                break;
            occ ^= lsbBB(bb);
            attackers |= (attacks::bishop(to, occ) & bishops) | (attacks::rook(to, occ) & rooks);
        } else {
            // Only the king is left to recapture: legal only if the opponent has no
            // remaining attacker to take it back.
            return !(attackers & board.us(~stm)).empty() ? (res ^ 1) != 0 : res != 0;
        }
    }

    return res != 0;
}

} // namespace engine
