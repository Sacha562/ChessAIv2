#include "perft.hpp"
#include "chess.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

using namespace chess;

namespace engine {
namespace Perft {

uint64_t perft(Board& board, int depth) {
    Movelist moves;
    movegen::legalmoves(moves, board);

    if (depth <= 1)
        return depth <= 0 ? 1 : moves.size();

    uint64_t nodes = 0;
    for (const auto& m : moves) {
        board.makeMove(m);
        nodes += perft(board, depth - 1);
        board.unmakeMove(m);
    }
    return nodes;
}

void divide(Board& board, int depth) {
    if (depth <= 0) {
        std::cout << "\nNodes: 1" << std::endl;
        return;
    }

    Movelist moves;
    movegen::legalmoves(moves, board);

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t total = 0;
    for (const auto& m : moves) {
        board.makeMove(m);
        const uint64_t n = (depth == 1) ? 1 : perft(board, depth - 1);
        board.unmakeMove(m);
        total += n;
        std::cout << uci::moveToUci(m) << ": " << n << '\n';
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    std::cout << "\nNodes: " << total << "  Time: " << ms << " ms";
    if (ms > 0)
        std::cout << "  NPS: " << (total * 1000ull / static_cast<uint64_t>(ms));
    std::cout << std::endl;
}

bool test() {
    struct Case { const char* fen; int depth; uint64_t nodes; };
    static const Case cases[] = {
        {constants::STARTPOS,                                                        6, 119060324ull},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",      5, 193690690ull}, // Kiwipete
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",                                 6,  11030083ull}, // Position 3
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",          5,  15833292ull}, // Position 4
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",                 5,  89941194ull}, // Position 5
        {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",  5, 164075551ull}, // Position 6
    };

    bool all = true;
    for (const auto& c : cases) {
        Board b(c.fen);
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t got = perft(b, c.depth);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        const bool ok = (got == c.nodes);
        all = all && ok;
        std::cout << (ok ? "[ OK ] " : "[FAIL] ")
                  << "depth " << c.depth
                  << "  expected " << c.nodes << "  got " << got
                  << "  (" << ms << " ms)  " << c.fen << '\n';
    }
    std::cout << (all ? "All perft tests passed." : "PERFT TESTS FAILED.") << std::endl;
    return all;
}

} // namespace Perft
} // namespace engine
