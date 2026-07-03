#include "bench.hpp"
#include "search.hpp"
#include "tt.hpp"
#include "chess.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>

using namespace chess;

namespace engine {
namespace Bench {

namespace {
constexpr int DEFAULT_DEPTH = 6;

// A small, fixed set of legal positions (opening / middlegame / endgame).
const char* const POSITIONS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 0 1",
    "rnbq1rk1/ppp1ppbp/3p1np1/8/2PPP3/2N2N2/PP2BPPP/R1BQK2R b KQ - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "6k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1",
    "8/8/8/4k3/8/8/4K3/R7 w - - 0 1",
    "rnbqkbnr/pp2pppp/3p4/2p5/3PP3/5N2/PPP2PPP/RNBQKB1R b KQkq - 0 3",
    "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2P2N2/PP1P1PPP/RNBQ1RK1 b kq - 0 5",
};
} // namespace

void run(int depth) {
    if (depth <= 0) depth = DEFAULT_DEPTH;

    std::atomic<bool> stop{false};
    TranspositionTable tt(16);
    uint64_t totalNodes = 0;

    const auto t0 = std::chrono::steady_clock::now();
    int idx = 0;
    for (const char* fen : POSITIONS) {
        Board board(fen);
        tt.clear();                       // per-position clear keeps the count deterministic
        Searcher searcher(stop, tt);
        Limits limits;
        limits.depth = depth;
        searcher.think(board, limits, /*printBest=*/false, /*printInfo=*/false);

        totalNodes += searcher.nodes();
        std::cout << "info string position " << ++idx
                  << " nodes " << searcher.nodes() << std::endl;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    const uint64_t nps = ms > 0 ? (totalNodes * 1000ull / static_cast<uint64_t>(ms))
                                : totalNodes;

    std::cout << "===========================\n";
    std::cout << totalNodes << " nodes " << nps << " nps" << std::endl;
}

} // namespace Bench
} // namespace engine
