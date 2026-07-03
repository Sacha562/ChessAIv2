// Unit tests for Static Exchange Evaluation (src/see.*). Strategy: hand-verified
// exchange sequences on crafted positions, each SEE value pinned by bracketing the
// threshold — `seeGE(m, v)` true and `seeGE(m, v + 1)` false proves SEE == v. Covers
// the classic bug surface: x-ray / battery reveals, en passant, promotion, and the
// king-may-only-take-when-undefended rule. SEE is minimax, so a side never makes a
// losing recapture; hence SEE never exceeds the first captured piece's value.
#include "doctest.h"

#include "chess.hpp"
#include "see.hpp"

using namespace engine;
using namespace chess;

namespace {

// SEE threshold query for `uciMove` in `fen`. Fails the test if the move is illegal.
bool seeGE_(const char* fen, const char* uciMove, int threshold) {
    Board      b(fen);
    const Move m = uci::uciToMove(b, uciMove);
    REQUIRE(m != Move(Move::NO_MOVE));
    return seeGE(b, m, threshold);
}

// Assert SEE(fen, move) == expected by bracketing the threshold.
void checkSee(const char* fen, const char* uciMove, int expected) {
    CHECK(seeGE_(fen, uciMove, expected));
    CHECK_FALSE(seeGE_(fen, uciMove, expected + 1));
}

} // namespace

TEST_CASE("pieceValue maps the material scale") {
    CHECK(pieceValue(PieceType::PAWN) == 100);
    CHECK(pieceValue(PieceType::KNIGHT) == 320);
    CHECK(pieceValue(PieceType::BISHOP) == 330);
    CHECK(pieceValue(PieceType::ROOK) == 500);
    CHECK(pieceValue(PieceType::QUEEN) == 900);
    CHECK(pieceValue(PieceType::NONE) == 0);
}

TEST_CASE("free capture of an undefended piece") {
    // Rook takes an undefended pawn: net +100, no recapture.
    checkSee("4k3/8/8/4p3/8/8/8/4R1K1 w - - 0 1", "e1e5", 100);
}

TEST_CASE("equal exchange nets zero") {
    // Nf3xe5 wins a knight; ...d6xe5 recaptures. 320 - 320 = 0.
    checkSee("4k3/8/3p4/4n3/8/5N2/8/4K3 w - - 0 1", "f3e5", 0);
}

TEST_CASE("losing capture: rook takes a pawn-defended pawn") {
    // Re1xe5 wins a pawn, ...d6xe5 wins the rook: 100 - 500 = -400.
    checkSee("4k3/8/3p4/4p3/8/8/8/4R1K1 w - - 0 1", "e1e5", -400);
}

TEST_CASE("battery / x-ray: doubled rooks recover the exchange") {
    // Re2xe5 (+100), ...Re8xe5 (-500), Re1xe5 (+500) — the rook behind the mover is
    // revealed once occupancy is peeled. A single-attacker SEE would read -400; the
    // x-ray rescan makes it +100.
    checkSee("4r2k/8/8/4p3/8/8/4R3/4R1K1 w - - 0 1", "e2e5", 100);
}

TEST_CASE("en passant capture") {
    // e5xd6 e.p. removes the d5 pawn (not a piece on d6). Undefended: +100.
    checkSee("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6", 100);
}

TEST_CASE("promotion capture scores the queening bonus") {
    // b7xa8=Q takes a knight (320) and promotes (+800). Undefended: 1120.
    checkSee("n3k3/1P6/8/8/8/8/8/4K3 w - - 0 1", "b7a8q", 1120);
}

TEST_CASE("king may not recapture into a defended square") {
    // d4xe5: e5 is defended only by the black king, but White's f4 pawn also covers
    // e5, so the king cannot take back. Net +100.
    checkSee("8/8/4k3/4p3/3P1P2/8/8/4K3 w - - 0 1", "d4e5", 100);
}

TEST_CASE("king recapture allowed when the square is undefended") {
    // Same as above without the f4 pawn: ...Kxe5 is legal, so 100 - 100 = 0.
    checkSee("8/8/4k3/4p3/3P4/8/8/4K3 w - - 0 1", "d4e5", 0);
}

TEST_CASE("quiet non-capture into a defended square is losing") {
    // Rd1-d5 is a quiet move onto a square guarded by the c6 pawn (pawns guard
    // diagonally, so c6 — not d6 — covers d5): the rook hangs, SEE < 0.
    CHECK_FALSE(seeGE_("4k3/8/2p5/8/8/8/8/3R1K2 w - - 0 1", "d1d5", 0));
}
