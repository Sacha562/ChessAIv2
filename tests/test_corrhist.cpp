// Unit tests for correction history (src/corrhist.*). Strategy: drive a
// CorrectionHistory through its public update API and assert the observable contract —
// an empty table corrects nothing, a residual moves the bucket in its own direction,
// buckets are keyed by pawn structure and side to move, the entry saturates at
// CORR_LIMIT, and a corrected eval never enters the mate range.
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

// Feed the same residual `diff` at `depth` `times` times, so the running average
// converges toward it.
void teach(CorrectionHistory& c, const Board& b, Value diff, int times, int depth = 8) {
    for (int i = 0; i < times; ++i)
        c.update(b, depth, diff);
}

} // namespace

TEST_CASE("a fresh CorrectionHistory corrects nothing") {
    const CorrectionHistory c;
    const Board             b(STARTPOS);
    CHECK(c.correction(b) == 0);
    CHECK(c.corrected(b, 123) == 123);
}

TEST_CASE("a residual moves the bucket in its own direction") {
    CorrectionHistory c;
    const Board       b(STARTPOS);

    teach(c, b, /*diff=*/40, /*times=*/8);
    CHECK(c.correction(b) > 0);
    CHECK(c.corrected(b, 0) == c.correction(b));

    CorrectionHistory down;
    teach(down, b, /*diff=*/-40, /*times=*/8);
    CHECK(down.correction(b) < 0);
}

TEST_CASE("correction is keyed by the pawn structure, not the pieces") {
    CorrectionHistory c;
    const Board       start(STARTPOS);
    const Board       noKnights(NO_KNIGHTS);
    const Board       advanced(E4);

    teach(c, start, /*diff=*/40, /*times=*/8);

    // Same pawns, different pieces -> same bucket (the correction is a structural prior).
    CHECK(pawnStructureKey(noKnights) == pawnStructureKey(start));
    CHECK(c.correction(noKnights) == c.correction(start));
    // A different pawn skeleton is a different bucket.
    CHECK(pawnStructureKey(advanced) != pawnStructureKey(start));
    CHECK(c.correction(advanced) == 0);
}

TEST_CASE("correction is keyed by the side to move") {
    CorrectionHistory c;
    const Board       white(STARTPOS);
    const Board       black("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");

    teach(c, white, /*diff=*/40, /*times=*/8);

    // Same pawn key, opposite side to move -> the other table, still untouched.
    REQUIRE(pawnStructureKey(black) == pawnStructureKey(white));
    CHECK(c.correction(white) > 0);
    CHECK(c.correction(black) == 0);
}

TEST_CASE("the running average saturates inside CORR_LIMIT") {
    CorrectionHistory c;
    const Board       b(STARTPOS);

    teach(c, b, /*diff=*/10000, /*times=*/200, /*depth=*/64);
    CHECK(c.correction(b) > 0);
    CHECK(c.correction(b) <= CORR_LIMIT / CORR_GRAIN);

    CorrectionHistory down;
    teach(down, b, /*diff=*/-10000, /*times=*/200, /*depth=*/64);
    CHECK(down.correction(b) >= -CORR_LIMIT / CORR_GRAIN);
}

TEST_CASE("a corrected eval never reaches the mate range") {
    CorrectionHistory c;
    const Board       b(STARTPOS);

    teach(c, b, /*diff=*/10000, /*times=*/200, /*depth=*/64);
    CHECK(!is_mate(c.corrected(b, VALUE_MATE_IN_MAX_PLY - 1)));
    CHECK(!is_mate(c.corrected(b, VALUE_MATED_IN_MAX_PLY + 1)));
}

TEST_CASE("clear resets every bucket") {
    CorrectionHistory c;
    const Board       b(STARTPOS);

    teach(c, b, /*diff=*/40, /*times=*/8);
    REQUIRE(c.correction(b) != 0);
    c.clear();
    CHECK(c.correction(b) == 0);
}
