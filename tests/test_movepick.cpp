// Unit tests for move ordering (src/movepick.*). Strategy: structural invariants
// that must hold for any position (the TT move leads; good captures precede quiets
// precede losing captures) plus direct MVV-LVA ranking checks. The bucket invariant
// is asserted by re-deriving each move's category (via the same isCapture / SEE
// signals the scorer uses) and checking the ordered list never steps back up.
#include "doctest.h"

#include "chess.hpp"
#include "history.hpp"
#include "movepick.hpp"
#include "see.hpp"

using namespace engine;
using namespace chess;

namespace {

// An empty heuristics table: no killers, no countermoves, all-zero history — so
// these ordering tests exercise the bucket structure, not the quiet signals.
const History NO_HIST;
const Move    NO_MOVE = Move(Move::NO_MOVE);

void order(const Board& b, Movelist& ml, Move ttMove) {
    orderMoves(b, ml, ttMove, NO_HIST, /*ply=*/0, /*prevMove=*/NO_MOVE, ContHistContext{});
}

// 2 = winning/equal capture (SEE >= 0), 1 = quiet, 0 = losing capture (SEE < 0).
int category(const Board& b, Move m) {
    if (!b.isCapture(m)) return 1;
    return seeGE(b, m, 0) ? 2 : 0;
}

Move parse(const Board& b, const char* s) {
    const Move m = uci::uciToMove(b, s);
    REQUIRE(m != Move(Move::NO_MOVE));
    return m;
}

const char* STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const char* KIWIPETE = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
const char* MIDGAME  = "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 0 1";

} // namespace

TEST_CASE("orderMoves groups good captures, then quiets, then losing captures") {
    for (const char* fen : {KIWIPETE, MIDGAME, STARTPOS}) {
        Board    b(fen);
        Movelist ml;
        movegen::legalmoves(ml, b);
        order(b, ml, NO_MOVE);

        int prev = 2; // categories descend 2 -> 1 -> 0 and never climb back
        for (int i = 0; i < ml.size(); ++i) {
            const int c = category(b, ml[i]);
            CHECK(c <= prev);
            prev = c;
        }
    }
}

TEST_CASE("orderMoves puts the TT move first") {
    Board    b(STARTPOS);
    Movelist ml;
    movegen::legalmoves(ml, b);
    REQUIRE(ml.size() > 1);

    const Move tt = ml[ml.size() - 1]; // a move that is not otherwise first
    order(b, ml, tt);
    CHECK(ml[0] == tt);
}

TEST_CASE("mvvLva ranks by victim value, then by (cheaper) attacker") {
    // White pawn c4 attacks queen b5 and rook d5; white queen d1 also attacks d5.
    Board b("4k3/8/8/1q1r4/2P5/8/8/3QK3 w - - 0 1");

    const int pxq = mvvLva(b, parse(b, "c4b5")); // pawn takes queen
    const int pxr = mvvLva(b, parse(b, "c4d5")); // pawn takes rook
    const int qxr = mvvLva(b, parse(b, "d1d5")); // queen takes rook

    CHECK(pxq > pxr); // most-valuable victim wins
    CHECK(pxr > qxr); // same victim: least-valuable attacker wins
}
