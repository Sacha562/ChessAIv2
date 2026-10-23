// Unit tests for NNUE inference (src/nnue.*). Strategy: property checks against a
// synthetic, deterministically random network, so no trained net file is needed — the
// incremental accumulator update must equal a from-scratch refresh after every move of
// random playouts (covering castling, en passant, promotions and captures), a position
// and its colour-mirrored twin must evaluate identically from the side to move, and the
// loader must reject missing or short files.
#include "doctest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <string>

#include "chess.hpp"
#include "nnue.hpp"

using namespace engine;
using namespace chess;

namespace {
constexpr int WEIGHT_RANGE  = 40; // small enough that no accumulator can overflow int16
constexpr int PLAYOUT_PLIES = 120;

std::unique_ptr<nnue::Network> randomNetwork(uint64_t seed) {
    auto                               net = std::make_unique<nnue::Network>();
    std::mt19937_64                    rng(seed);
    std::uniform_int_distribution<int> w(-WEIGHT_RANGE, WEIGHT_RANGE);
    for (auto& column : net->featureWeights)
        for (auto& v : column)
            v = static_cast<int16_t>(w(rng));
    for (auto& v : net->featureBias)
        v = static_cast<int16_t>(w(rng) + WEIGHT_RANGE); // mostly positive: SCReLU is live
    for (auto& v : net->outputWeights)
        v = static_cast<int16_t>(w(rng));
    net->outputBias = static_cast<int16_t>(w(rng));
    return net;
}

// Positions whose playouts reach castling, en passant and promotions early.
const char* START_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "8/2P5/8/8/8/8/2p5/K6k w - - 0 1",
};
} // namespace

TEST_CASE("incremental updates match a full refresh through random playouts") {
    const auto      net = randomNetwork(1);
    std::mt19937_64 rng(7);
    int             checked = 0;
    for (const char* fen : START_FENS) {
        for (int game = 0; game < 8; ++game) {
            Board             board(fen);
            nnue::Accumulator acc;
            nnue::refresh(*net, board, acc);
            for (int ply = 0; ply < PLAYOUT_PLIES; ++ply) {
                Movelist moves;
                movegen::legalmoves(moves, board);
                if (moves.empty()) break;
                const Move        m = moves[static_cast<int>(rng() % moves.size())];
                nnue::Accumulator child;
                nnue::update(*net, board, m, acc, child);
                board.makeMove(m);

                nnue::Accumulator fresh;
                nnue::refresh(*net, board, fresh);
                REQUIRE(child.values == fresh.values);
                acc = child;
                ++checked;
            }
        }
    }
    CHECK(checked > 1000);
}

TEST_CASE("a colour-mirrored position evaluates the same for the side to move") {
    const auto                                net     = randomNetwork(2);
    const std::pair<const char*, const char*> pairs[] = {
        {"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
         "rnbqkbnr/pppp1ppp/8/4p3/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
         "rnbqk2r/pppp1ppp/5n2/2b1p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R b KQkq - 4 4"},
    };
    for (const auto& [fen, mirrored] : pairs) {
        const Board       a(fen), b(mirrored);
        nnue::Accumulator accA, accB;
        nnue::refresh(*net, a, accA);
        nnue::refresh(*net, b, accB);
        CHECK(nnue::evaluate(*net, accA, a.sideToMove()) ==
              nnue::evaluate(*net, accB, b.sideToMove()));
    }
}

TEST_CASE("loadNetwork rejects a missing or short file") {
    CHECK(nnue::loadNetwork("no-such-net.bin") == nullptr);

    const std::string path = "test_nnue_short.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "too short";
    }
    CHECK(nnue::loadNetwork(path) == nullptr);
    std::remove(path.c_str());
}

TEST_CASE("the embedded default network loads and judges material sensibly") {
    // Built with the repo's default EVALFILE; a build that embeds nothing skips the checks.
    const nnue::Network* net = nnue::embeddedNetwork();
    if (net == nullptr) return;
    CHECK(nnue::embeddedNetwork() == net); // built once, then shared

    const auto eval = [net](const char* fen) {
        const Board       board(fen);
        nnue::Accumulator acc;
        nnue::refresh(*net, board, acc);
        return nnue::evaluate(*net, acc, board.sideToMove());
    };
    const Value start = eval("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(std::abs(start) < 100);
    CHECK(eval("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") > 800);
    CHECK(eval("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNB1KBNR w KQkq - 0 1") < -800);
}
