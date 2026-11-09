// Unit tests for correction history (src/corrhist.*). Strategy: drive a
// CorrectionHistory through its public slots / update API and assert the observable
// contract — an empty table corrects nothing, a residual moves the correction in its own
// direction, each table is keyed by its own piece subset and the side to move, weights
// switch tables on and off, the gravity update converges instead of overshooting, every
// entry saturates at CORR_LIMIT, and a corrected eval never enters the mate range.
#include "doctest.h"

#include "chess.hpp"
#include "corrhist.hpp"

using namespace engine;
using namespace chess;

namespace {

const char* STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
// Same position with the knights removed: identical pawns, different pieces.
const char* NO_KNIGHTS = "r1bqkb1r/pppppppp/8/8/8/8/PPPPPPPP/R1BQKB1R w KQkq - 0 1";
// A different pawn skeleton (White's e-pawn advanced).
const char* E4 = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1";

constexpr int TABLES = 6; // pawn, non-pawn x2, minor, major, continuation

// Table weights in percent, in CorrWeights order; 0 switches a table's read off.
CorrWeights only(int pawn, int nonPawn, int minor, int major, int cont) {
    return CorrWeights{pawn, nonPawn, minor, major, cont};
}

CorrSlots slotsOf(const Board& b, int prevPiece = -1, int prevTo = 0) {
    return CorrectionHistory::slots(b, prevPiece, prevTo);
}

// Feed the same residual `diff` at `depth` `times` times.
void teach(CorrectionHistory& c, const CorrSlots& s, Value diff, int times, int depth = 8) {
    for (int i = 0; i < times; ++i)
        c.update(s, depth, diff);
}

} // namespace

TEST_CASE("a fresh CorrectionHistory corrects nothing") {
    const CorrectionHistory c;
    const CorrSlots         s = slotsOf(Board(STARTPOS), 3, 20);
    CHECK(c.correction(s) == 0);
    CHECK(c.corrected(s, 123) == 123);
}

TEST_CASE("a residual moves the correction in its own direction") {
    CorrectionHistory c;
    const CorrSlots   s = slotsOf(Board(STARTPOS));

    teach(c, s, /*diff=*/40, /*times=*/8);
    CHECK(c.correction(s) > 0);
    CHECK(c.corrected(s, 0) == c.correction(s));

    CorrectionHistory down;
    teach(down, s, /*diff=*/-40, /*times=*/8);
    CHECK(down.correction(s) < 0);
}

TEST_CASE("the pawn table is keyed by the pawn structure, not the pieces") {
    CorrectionHistory c;
    c.setWeights(only(100, 0, 0, 0, 0));
    const Board start(STARTPOS);
    const Board noKnights(NO_KNIGHTS);
    const Board advanced(E4);

    teach(c, slotsOf(start), /*diff=*/40, /*times=*/8);

    // Same pawns, different pieces -> same bucket (the correction is a structural prior).
    CHECK(pawnStructureKey(noKnights) == pawnStructureKey(start));
    CHECK(c.correction(slotsOf(noKnights)) == c.correction(slotsOf(start)));
    // A different pawn skeleton is a different bucket.
    CHECK(pawnStructureKey(advanced) != pawnStructureKey(start));
    CHECK(c.correction(slotsOf(advanced)) == 0);
}

TEST_CASE("piece keys see their own pieces only") {
    const Board start(STARTPOS);
    const Board noKnights(NO_KNIGHTS);
    const Board advanced(E4);

    // Knights are non-pawn and minor pieces, not major ones.
    CHECK(nonPawnKey(noKnights, Color::WHITE) != nonPawnKey(start, Color::WHITE));
    CHECK(minorKey(noKnights) != minorKey(start));
    CHECK(majorKey(noKnights) == majorKey(start));

    // A pawn move changes none of the piece keys.
    CHECK(nonPawnKey(advanced, Color::WHITE) == nonPawnKey(start, Color::WHITE));
    CHECK(nonPawnKey(advanced, Color::BLACK) == nonPawnKey(start, Color::BLACK));
    CHECK(minorKey(advanced) == minorKey(start));
    CHECK(majorKey(advanced) == majorKey(start));

    // Each colour's non-pawn key is its own.
    CHECK(nonPawnKey(start, Color::WHITE) != nonPawnKey(start, Color::BLACK));
}

TEST_CASE("a table with weight 0 is never read") {
    CorrectionHistory c;
    c.setWeights(only(0, 0, 0, 0, 0));
    const CorrSlots s = slotsOf(Board(STARTPOS), 3, 20);
    teach(c, s, /*diff=*/40, /*times=*/8);
    CHECK(c.correction(s) == 0);

    c.setWeights(only(0, 0, 0, 0, 100));
    CHECK(c.correction(s) > 0); // it learned all along; only the read was off
}

TEST_CASE("the continuation table is keyed by the previous move") {
    CorrectionHistory c;
    c.setWeights(only(0, 0, 0, 0, 100));
    const Board b(STARTPOS);

    teach(c, slotsOf(b, /*prevPiece=*/3, /*prevTo=*/20), /*diff=*/40, /*times=*/8);
    CHECK(c.correction(slotsOf(b, 3, 20)) > 0);
    CHECK(c.correction(slotsOf(b, 3, 21)) == 0);
    CHECK(c.correction(slotsOf(b)) == 0); // no previous move reads nothing
}

TEST_CASE("every table is keyed by the side to move") {
    CorrectionHistory c;
    const Board       white(STARTPOS);
    const Board       black("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");

    teach(c, slotsOf(white, 3, 20), /*diff=*/40, /*times=*/8);

    // Same keys, opposite side to move -> the other half of every table, still untouched.
    REQUIRE(pawnStructureKey(black) == pawnStructureKey(white));
    CHECK(c.correction(slotsOf(white, 3, 20)) > 0);
    CHECK(c.correction(slotsOf(black, 3, 20)) == 0);
}

TEST_CASE("the tables converge on the residual together") {
    CorrectionHistory c;
    const CorrSlots   s = slotsOf(Board(STARTPOS), 3, 20);

    // The search keeps scoring 30 cp above the raw eval. Each update teaches the gap left
    // after the current correction, so the summed correction settles near 30 cp rather
    // than every table claiming all of it.
    constexpr Value TRUE_GAP = 30;
    for (int i = 0; i < 400; ++i)
        c.update(s, /*depth=*/8, TRUE_GAP - c.correction(s));
    CHECK(c.correction(s) >= TRUE_GAP - 3);
    CHECK(c.correction(s) <= TRUE_GAP + 3);
}

TEST_CASE("each entry saturates inside CORR_LIMIT") {
    CorrectionHistory c;
    const CorrSlots   s = slotsOf(Board(STARTPOS), 3, 20);

    teach(c, s, /*diff=*/10000, /*times=*/200, /*depth=*/64);
    CHECK(c.correction(s) > 0);
    CHECK(c.correction(s) <= TABLES * CORR_LIMIT / CORR_GRAIN);

    c.setWeights(only(100, 0, 0, 0, 0));
    CHECK(c.correction(s) <= CORR_LIMIT / CORR_GRAIN);

    CorrectionHistory down;
    teach(down, s, /*diff=*/-10000, /*times=*/200, /*depth=*/64);
    CHECK(down.correction(s) >= -TABLES * CORR_LIMIT / CORR_GRAIN);
}

TEST_CASE("a corrected eval never reaches the mate range") {
    CorrectionHistory c;
    const CorrSlots   s = slotsOf(Board(STARTPOS), 3, 20);

    teach(c, s, /*diff=*/10000, /*times=*/200, /*depth=*/64);
    CHECK(!is_mate(c.corrected(s, VALUE_MATE_IN_MAX_PLY - 1)));
    CHECK(!is_mate(c.corrected(s, VALUE_MATED_IN_MAX_PLY + 1)));
}

TEST_CASE("clear resets every bucket") {
    CorrectionHistory c;
    const CorrSlots   s = slotsOf(Board(STARTPOS), 3, 20);

    teach(c, s, /*diff=*/40, /*times=*/8);
    REQUIRE(c.correction(s) != 0);
    c.clear();
    CHECK(c.correction(s) == 0);
}
