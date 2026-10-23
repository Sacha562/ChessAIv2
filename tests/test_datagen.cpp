// Unit tests for datagen's game adjudication (src/datagen.*). Strategy: feed scripted
// white-relative score sequences into an Adjudicator and check exactly when it calls a
// win or a draw — streak lengths, streak resets, the side of the win, and the minimum
// game ply for draws. The game loop and threads are exercised end to end by a real
// `chessai datagen` run instead (see build.md).
#include "doctest.h"

#include "datagen.hpp"

using namespace engine;

namespace {
using viriformat::Outcome;

constexpr int LATE_PLY = Adjudicator::DRAW_MIN_PLY + 10;
} // namespace

TEST_CASE("a win is called after WIN_PLIES consecutive decided plies") {
    Adjudicator adj;
    for (int i = 1; i < Adjudicator::WIN_PLIES; ++i)
        CHECK_FALSE(adj.update(Adjudicator::WIN_SCORE, 20 + i).has_value());
    CHECK(adj.update(Adjudicator::WIN_SCORE + 500, 30) == Outcome::WhiteWin);

    Adjudicator black;
    for (int i = 1; i < Adjudicator::WIN_PLIES; ++i)
        CHECK_FALSE(black.update(-Adjudicator::WIN_SCORE, 20 + i).has_value());
    CHECK(black.update(-Adjudicator::WIN_SCORE, 30) == Outcome::BlackWin);
}

TEST_CASE("an interrupted winning streak starts over") {
    Adjudicator adj;
    for (int i = 1; i < Adjudicator::WIN_PLIES; ++i)
        adj.update(Adjudicator::WIN_SCORE, i);
    CHECK_FALSE(adj.update(Adjudicator::WIN_SCORE - 1, 10).has_value());
    for (int i = 1; i < Adjudicator::WIN_PLIES; ++i)
        CHECK_FALSE(adj.update(Adjudicator::WIN_SCORE, 10 + i).has_value());
    CHECK(adj.update(Adjudicator::WIN_SCORE, 20) == Outcome::WhiteWin);
}

TEST_CASE("a draw needs DRAW_PLIES level plies past DRAW_MIN_PLY") {
    Adjudicator early;
    for (int i = 0; i < 2 * Adjudicator::DRAW_PLIES; ++i)
        CHECK_FALSE(early.update(0, Adjudicator::DRAW_MIN_PLY - 2 * Adjudicator::DRAW_PLIES + i)
                        .has_value());

    Adjudicator late;
    for (int i = 1; i < Adjudicator::DRAW_PLIES; ++i)
        CHECK_FALSE(late.update(Adjudicator::DRAW_SCORE, LATE_PLY + i).has_value());
    CHECK(late.update(-Adjudicator::DRAW_SCORE, LATE_PLY + 20) == Outcome::Draw);
}

TEST_CASE("a non-level score breaks the draw streak") {
    Adjudicator adj;
    for (int i = 1; i < Adjudicator::DRAW_PLIES; ++i)
        adj.update(0, LATE_PLY + i);
    CHECK_FALSE(adj.update(Adjudicator::DRAW_SCORE + 1, LATE_PLY + 20).has_value());
    CHECK_FALSE(adj.update(0, LATE_PLY + 21).has_value());
}
