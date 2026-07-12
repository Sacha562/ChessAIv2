// Functional tests for the Phase 1c main-search extensions (singular extensions),
// exercised through the public Searcher. Singular extensions change how the search
// *allocates depth*, not the result on a clear tactic, so they are validated three
// ways: the engine still finds the forcing move in known tactical / mate positions with
// singular ON; toggling UseSingular never changes the move it settles on for those
// forced positions (a correctness-regression guard); and lowering the trigger depth so
// singular fires heavily still leaves the forced move intact. Deterministic — fixed
// depth, single thread, no clock.
#include "doctest.h"

#include <atomic>

#include "chess.hpp"
#include "search.hpp"
#include "tt.hpp"

using namespace engine;
using namespace chess;

namespace {

// Best move of a fixed-depth search from `fen`, as UCI text, under the given tunables.
// Silent (no info/bestmove printing), fresh TT per call for determinism.
std::string bestMove(const char* fen, int depth, const Tunables& tp) {
    std::atomic<bool>  stop{false};
    TranspositionTable tt(8);
    Searcher           searcher(stop, tt, tp);

    Limits limits;
    limits.depth    = depth;
    const Move best = searcher.think(Board(fen), limits, /*printBest=*/false, /*printInfo=*/false);
    return uci::moveToUci(best);
}

// Tunables with singular extensions on (the default) / off.
Tunables singular(bool on) {
    Tunables tp;
    tp.useSingular = on;
    return tp;
}

// Positions with a single forcing best move, deep enough that singular extensions run
// (default trigger depth is 8).
const char* BACK_RANK_MATE = "6k1/5ppp/8/8/8/8/8/R6K w - - 0 1";  // Ra8#
const char* HANGING_QUEEN  = "4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1"; // exd5 wins the queen
const char* FREE_KNIGHT    = "4k3/8/8/3n4/4P3/8/8/4K3 w - - 0 1"; // exd5 wins the knight

} // namespace

TEST_CASE("singular search still finds the back-rank mate") {
    CHECK(bestMove(BACK_RANK_MATE, 8, singular(true)) == "a1a8");
}

TEST_CASE("singular search still wins the hanging queen") {
    CHECK(bestMove(HANGING_QUEEN, 8, singular(true)) == "e4d5");
}

TEST_CASE("toggling singular does not change a forced tactic") {
    for (const char* fen : {BACK_RANK_MATE, HANGING_QUEEN, FREE_KNIGHT}) {
        CHECK(bestMove(fen, 8, singular(true)) == bestMove(fen, 8, singular(false)));
    }
}

TEST_CASE("a low singular trigger depth still solves the forced tactic") {
    // Force the singular path to fire on nearly every node, then confirm the forcing
    // move survives — a guard that the verification search and its exclusion plumbing do
    // not corrupt the result.
    Tunables heavy         = singular(true);
    heavy.singularMinDepth = 4;
    CHECK(bestMove(HANGING_QUEEN, 8, heavy) == "e4d5");
    CHECK(bestMove(BACK_RANK_MATE, 8, heavy) == "a1a8");
}

TEST_CASE("singular search is deterministic") {
    CHECK(bestMove(HANGING_QUEEN, 9, singular(true)) == bestMove(HANGING_QUEEN, 9, singular(true)));
}
