// Functional tests for quiescence search + ordering (src/search.*, exercised through
// the public Searcher). Quiescence has no score accessor, so it is validated by the
// move it makes: a fixed-depth search must grab free material, must NOT walk into a
// capture that loses to the recapture (the horizon effect qsearch exists to cure),
// and must still see a forced mate. Deterministic — fixed depth, single thread, no
// clock.
#include "doctest.h"

#include <atomic>

#include "chess.hpp"
#include "search.hpp"
#include "tt.hpp"

using namespace engine;
using namespace chess;

namespace {

// Best move of a fixed-depth search from `fen`, as UCI text. Silent (no info/bestmove
// printing), fresh TT per call for determinism.
std::string bestMove(const char* fen, int depth) {
    std::atomic<bool>  stop{false};
    TranspositionTable tt(8);
    Searcher           searcher(stop, tt);

    Limits limits;
    limits.depth = depth;
    const Move best = searcher.think(Board(fen), limits, /*printBest=*/false, /*printInfo=*/false);
    return uci::moveToUci(best);
}

} // namespace

TEST_CASE("captures a hanging queen") {
    // e4xd5 wins the undefended queen; nothing else comes close.
    CHECK(bestMove("4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1", 2) == "e4d5");
}

TEST_CASE("does not grab a pawn-defended pawn that loses the queen") {
    // Qd1xd5?? wins a pawn but hangs the queen to ...e6xd5. A depth-1 static search
    // sees only +100 and blunders; quiescence resolves the recapture, so the queen
    // must not take on d5.
    CHECK(bestMove("4k3/8/4p3/3p4/8/8/8/3QK3 w - - 0 1", 3) != "d1d5");
}

TEST_CASE("wins a free undefended knight") {
    // e4xd5 captures the undefended knight; nothing is better.
    CHECK(bestMove("4k3/8/8/3n4/4P3/8/8/4K3 w - - 0 1", 2) == "e4d5");
}

TEST_CASE("still finds a forced mate in one") {
    // Ra1-a8 is mate; the king is boxed in by its own pawns.
    CHECK(bestMove("6k1/5ppp/8/8/8/8/8/R6K w - - 0 1", 3) == "a1a8");
}
