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
    std::atomic<SearchState> state{SearchState::Running};
    TranspositionTable       tt(8);
    Searcher                 searcher(state, tt, tp);

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

TEST_CASE("razoring and ProbCut leave a forced tactic intact") {
    // Both ship off; turning them on must reshape the tree without changing the verdict
    // on a position whose best move is forced.
    Tunables on;
    on.useRazoring = true;
    on.useProbCut  = true;

    for (const char* fen : {HANGING_QUEEN, FREE_KNIGHT}) {
        CHECK(bestMove(fen, 8, on) == bestMove(fen, 8, Tunables{}));
    }
}

TEST_CASE("an eager razoring gate still wins the hanging piece") {
    // Widen the razoring gate far past its default so it fires on almost every shallow
    // node: the q-search verification must still refuse to prune a winning capture.
    Tunables eager    = Tunables{};
    eager.useRazoring = true;
    eager.razorBase   = 600;
    eager.razorMargin = 400;
    CHECK(bestMove(HANGING_QUEEN, 8, eager) == "e4d5");
    CHECK(bestMove(FREE_KNIGHT, 8, eager) == "e4d5");
}

TEST_CASE("an eager ProbCut still wins the hanging piece") {
    // Lowest trigger depth and a thin margin, so the shallow capture search runs
    // constantly; the forced capture must survive.
    Tunables eager        = Tunables{};
    eager.useProbCut      = true;
    eager.probCutMinDepth = 3;
    eager.probCutMargin   = 50;
    CHECK(bestMove(HANGING_QUEEN, 8, eager) == "e4d5");
    CHECK(bestMove(FREE_KNIGHT, 8, eager) == "e4d5");
}

TEST_CASE("a soft node budget finishes the iteration that crosses it") {
    // Datagen's per-move budget: the search must spend at least the soft budget, stop
    // at an iteration boundary, and never touch the (much larger) hard cap.
    std::atomic<SearchState> state{SearchState::Running};
    TranspositionTable       tt(8);
    Searcher                 searcher(state, tt);

    Limits limits;
    limits.softNodes = 5000;
    limits.nodes     = 1'000'000;
    searcher.think(Board(), limits, /*printBest=*/false, /*printInfo=*/false);
    CHECK(searcher.nodes() >= 5000);
    CHECK(searcher.nodes() < 1'000'000);
}

TEST_CASE("score reports the last completed iteration") {
    std::atomic<SearchState> state{SearchState::Running};
    TranspositionTable       tt(8);
    Searcher                 searcher(state, tt);

    Limits limits;
    limits.depth = 4;
    searcher.think(Board(BACK_RANK_MATE), limits, /*printBest=*/false, /*printInfo=*/false);
    CHECK(searcher.score() == mate_in(1));

    searcher.think(Board(HANGING_QUEEN), limits, /*printBest=*/false, /*printInfo=*/false);
    // exd5 leaves a lone pawn against a bare king: a clearly positive, non-mate score
    // (and proof the mate score of the previous search was replaced).
    CHECK(searcher.score() > 0);
    CHECK_FALSE(is_mate(searcher.score()));
}
