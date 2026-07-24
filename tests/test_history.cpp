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
// quiets in `tried` searched (and failed) first. `prev` is the opponent's last move;
// `ctx` is the continuation context (empty by default, so continuation history is
// untouched unless a test supplies one).
void cutoff(History& h, const Board& b, Move move, Move prev, std::initializer_list<Move> tried,
            int depth = 6, const ContHistContext& ctx = {}) {
    Move buf[16];
    int  n = 0;
    for (const Move m : tried)
        buf[n++] = m;
    h.updateQuietCutoff(b, b.sideToMove(), /*ply=*/0, depth, prev, move, buf, n, ctx);
}

// Rank of `m` in the ordered list, or -1 if absent.
int rankOf(const Movelist& ml, Move m) {
    for (int i = 0; i < ml.size(); ++i)
        if (ml[i] == m) return i;
    return -1;
}

// Colour+type index (0–11) of the piece that plays `m` on `b` — the key a continuation
// entry uses for the current move.
int pieceIndex(const Board& b, Move m) {
    return static_cast<int>(b.at(m.from()));
}

// A continuation context keying on a fabricated "move one ply ago" (piece `p`, square
// `to`). Only the indices matter for exercising the table.
ContHistContext ctx1(int p, int to) {
    ContHistContext c;
    c.piece1 = p;
    c.to1    = to;
    return c;
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
    orderMoves(b, ml, NONE, h, /*ply=*/0, /*prevMove=*/NONE, ContHistContext{});
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
    orderMoves(b, ml, NONE, h, /*ply=*/0, /*prevMove=*/NONE, ContHistContext{});

    const Move neutral = parse(b, "g1f3"); // untouched: zero history, not a killer
    CHECK(rankOf(ml, neutral) < rankOf(ml, penalis));
}

TEST_CASE("continuation history rewards the cutoff and penalises the quiets tried first") {
    History    h;
    Board      b(STARTPOS);
    const Move cut  = parse(b, "e2e3");
    const Move fail = parse(b, "d2d3");
    const auto ctx  = ctx1(/*piece=*/1, /*to=*/20); // some prior move one ply back

    cutoff(h, b, cut, NONE, {fail}, /*depth=*/6, ctx);

    CHECK(h.continuationScore(ctx, pieceIndex(b, cut), cut.to().index()) > 0);
    CHECK(h.continuationScore(ctx, pieceIndex(b, fail), fail.to().index()) < 0);
}

TEST_CASE("continuation history is keyed by the recent-move context") {
    History    h;
    Board      b(STARTPOS);
    const Move cut = parse(b, "e2e3");
    const auto ctx = ctx1(/*piece=*/1, /*to=*/20);

    cutoff(h, b, cut, NONE, {}, /*depth=*/6, ctx);

    // Same move, different prior move -> that continuation slot was never touched.
    const auto other = ctx1(/*piece=*/2, /*to=*/20);
    CHECK(h.continuationScore(other, pieceIndex(b, cut), cut.to().index()) == 0);
    // Empty context (no prior move) -> no continuation contribution at all.
    CHECK(h.continuationScore(ContHistContext{}, pieceIndex(b, cut), cut.to().index()) == 0);
}

TEST_CASE("continuation history is silent when disabled") {
    History h;
    h.setEnabled(/*killers=*/true, /*history=*/true, /*countermove=*/true, /*contHist=*/false);
    Board      b(STARTPOS);
    const Move cut = parse(b, "e2e3");
    const auto ctx = ctx1(/*piece=*/1, /*to=*/20);

    cutoff(h, b, cut, NONE, {}, /*depth=*/6, ctx);
    CHECK(h.continuationScore(ctx, pieceIndex(b, cut), cut.to().index()) == 0);
}

TEST_CASE("a continuation-history quiet floats above a neutral quiet") {
    History h;
    // Isolate continuation history: turn off the other quiet signals so only it can
    // move a quiet's rank.
    h.setEnabled(/*killers=*/false, /*history=*/false, /*countermove=*/false, /*contHist=*/true);
    Board      b(STARTPOS);
    const auto ctx  = ctx1(/*piece=*/3, /*to=*/30);
    const Move good = parse(b, "e2e3");

    cutoff(h, b, good, NONE, {}, /*depth=*/6, ctx); // reward `good` under this context

    Movelist ml;
    movegen::legalmoves(ml, b);
    orderMoves(b, ml, NONE, h, /*ply=*/0, /*prevMove=*/NONE, ctx);

    const Move neutral = parse(b, "a2a3"); // never rewarded under `ctx`
    CHECK(rankOf(ml, good) < rankOf(ml, neutral));
}
