// Unit tests for the quiet-move ordering heuristics (src/history.*). Strategy: drive
// History through its update API and assert the observable contract — killer LIFO,
// history bonus/malus signs, the saturating gravity bound, and countermove
// round-trips — then a couple of end-to-end checks that the signals actually move a
// quiet's rank in movepick's ordering.
#include "doctest.h"

#include "chess.hpp"
#include "history.hpp"
#include "movepick.hpp"

#include <initializer_list>

using namespace engine;
using namespace chess;

namespace {

const char* STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const Move  NONE     = Move(Move::NO_MOVE);

Move parse(const Board& b, const char* s) {
    const Move m = uci::uciToMove(b, s);
    REQUIRE(m != NONE);
    return m;
}

// Record a quiet cutoff by `cutoff` at ply 0 for the side to move on `b`, with the
// quiets in `tried` searched (and failed) first. `prev` is the opponent's last move.
void cutoff(History& h, const Board& b, Move move, Move prev, std::initializer_list<Move> tried,
            int depth = 6) {
    Move buf[16];
    int  n = 0;
    for (const Move m : tried)
        buf[n++] = m;
    h.updateQuietCutoff(b, b.sideToMove(), /*ply=*/0, depth, prev, move, buf, n);
}

// Rank of `m` in the ordered list, or -1 if absent.
int rankOf(const Movelist& ml, Move m) {
    for (int i = 0; i < ml.size(); ++i)
        if (ml[i] == m) return i;
    return -1;
}

} // namespace

TEST_CASE("a fresh History is empty") {
    const History h;
    Board         b(STARTPOS);
    CHECK(h.quietScore(Color::WHITE, parse(b, "e2e3")) == 0);
    CHECK(h.killer(0, 0) == NONE);
    CHECK(h.killer(0, 1) == NONE);
    CHECK(h.counter(b, parse(b, "e2e4")) == NONE);
}

TEST_CASE("killers keep the two most recent distinct cutoffs, newest first") {
    History    h;
    Board      b(STARTPOS);
    const Move e3 = parse(b, "e2e3");
    const Move d3 = parse(b, "d2d3");

    cutoff(h, b, e3, NONE, {});
    CHECK(h.killer(0, 0) == e3);

    cutoff(h, b, d3, NONE, {});
    CHECK(h.killer(0, 0) == d3); // newest in slot 0
    CHECK(h.killer(0, 1) == e3); // previous shifted to slot 1

    cutoff(h, b, d3, NONE, {}); // same move again: no shuffle, no duplicate
    CHECK(h.killer(0, 0) == d3);
    CHECK(h.killer(0, 1) == e3);
}

TEST_CASE("history rewards the cutoff move and penalises the quiets tried first") {
    History    h;
    Board      b(STARTPOS);
    const Move cut  = parse(b, "e2e3");
    const Move fail = parse(b, "d2d3");

    cutoff(h, b, cut, NONE, {fail});
    CHECK(h.quietScore(Color::WHITE, cut) > 0);
    CHECK(h.quietScore(Color::WHITE, fail) < 0);
}

TEST_CASE("history saturates within +/-MAX_HISTORY under repeated updates") {
    History    h;
    Board      b(STARTPOS);
    const Move up   = parse(b, "e2e3");
    const Move down = parse(b, "d2d3");

    for (int i = 0; i < 1000; ++i)
        cutoff(h, b, up, NONE, {down}, /*depth=*/64);

    const int hi = h.quietScore(Color::WHITE, up);
    const int lo = h.quietScore(Color::WHITE, down);
    CHECK(hi > 0);
    CHECK(lo < 0);
    CHECK(hi <= MAX_HISTORY);
    CHECK(lo >= -MAX_HISTORY);
}

TEST_CASE("countermove stores and retrieves the refutation keyed by the previous move") {
    History    h;
    Board      b(STARTPOS);
    const Move e4 = parse(b, "e2e4");
    b.makeMove(e4); // b is now the position after e2e4
    const Move reply = parse(b, "e7e5");

    cutoff(h, b, reply, e4, {});
    CHECK(h.counter(b, e4) == reply);  // e7e5 is the stored refutation of e2e4
    CHECK(h.counter(b, NONE) == NONE); // no previous move -> no countermove
}

TEST_CASE("a killer move floats above the ordinary quiets") {
    History    h;
    Board      b(STARTPOS);
    const Move killer = parse(b, "g1f3");
    cutoff(h, b, killer, NONE, {}); // make g1f3 the ply-0 killer

    Movelist ml;
    movegen::legalmoves(ml, b);
    orderMoves(b, ml, NONE, h, /*ply=*/0, /*prevMove=*/NONE);
    CHECK(ml[0] == killer); // no captures at startpos -> killer leads
}

TEST_CASE("a history-penalised quiet sinks below a neutral quiet") {
    History    h;
    Board      b(STARTPOS);
    const Move cut     = parse(b, "d2d3"); // becomes killer + positive history
    const Move penalis = parse(b, "b1c3"); // tried first, fails -> negative history
    cutoff(h, b, cut, NONE, {penalis});

    Movelist ml;
    movegen::legalmoves(ml, b);
    orderMoves(b, ml, NONE, h, /*ply=*/0, /*prevMove=*/NONE);

    const Move neutral = parse(b, "g1f3"); // untouched: zero history, not a killer
    CHECK(rankOf(ml, neutral) < rankOf(ml, penalis));
}
