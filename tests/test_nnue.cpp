// Unit tests for NNUE inference (src/nnue.*). Strategy: property checks against a
// synthetic, deterministically random network, so no trained net file is needed — the
// incremental accumulator update must equal a from-scratch refresh after every move of
// random playouts (covering castling, en passant, promotions and captures), a position
// and its colour-mirrored twin must evaluate identically from the side to move, the output
// row must follow the piece count, the (vectorised) output layer must equal the plain
// formula at full weight range, and the loaders must reject missing or short files and
// output weights too large for the int16 product.
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
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
// The largest output weight nnue accepts: clamp(x) * w must fit int16.
constexpr int MAX_OUTPUT_WEIGHT = std::numeric_limits<int16_t>::max() / nnue::QA;
// Synthetic accumulator lanes: about a third negative (SCReLU 0), a third above QA.
constexpr int ACC_LOW  = -200;
constexpr int ACC_HIGH = 450;

std::unique_ptr<nnue::Network> randomNetwork(uint64_t seed) {
    auto                               net = std::make_unique<nnue::Network>();
    std::mt19937_64                    rng(seed);
    std::uniform_int_distribution<int> w(-WEIGHT_RANGE, WEIGHT_RANGE);
    for (auto& column : net->featureWeights)
        for (auto& v : column)
            v = static_cast<int16_t>(w(rng));
    for (auto& v : net->featureBias)
        v = static_cast<int16_t>(w(rng) + WEIGHT_RANGE); // mostly positive: SCReLU is live
    for (auto& row : net->outputWeights)
        for (auto& v : row)
            v = static_cast<int16_t>(w(rng));
    for (auto& v : net->outputBias)
        v = static_cast<int16_t>(w(rng));
    return net;
}

// Positions whose playouts reach castling, en passant and promotions early.
const char* START_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "8/2P5/8/8/8/8/2p5/K6k w - - 0 1",
    "8/2p5/3k4/8/8/4K3/5P2/8 w - - 0 1", // central kings: bucket and d/e mirror crossings
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
        CHECK(nnue::evaluate(*net, accA, a) == nnue::evaluate(*net, accB, b));
    }
}

TEST_CASE("a file-mirrored position evaluates the same") {
    // Each perspective mirrors a<->h whenever its king is on files e-h, so a position and
    // its left-right mirror (no castling rights) present identical features.
    const auto                                net     = randomNetwork(6);
    const std::pair<const char*, const char*> pairs[] = {
        {"8/2p5/3k4/8/8/4K3/5P2/8 w - - 0 1", "8/5p2/4k3/8/8/3K4/2P5/8 w - - 0 1"},
        {"r4rk1/pp3ppp/2n5/3q4/8/2N5/PP3PPP/R2Q1RK1 b - - 0 15",
         "1kr4r/ppp3pp/5n2/4q3/8/5N2/PPP3PP/1KR1Q2R b - - 0 15"},
    };
    for (const auto& [fen, mirrored] : pairs) {
        const Board       a(fen), b(mirrored);
        nnue::Accumulator accA, accB;
        nnue::refresh(*net, a, accA);
        nnue::refresh(*net, b, accB);
        CHECK(accA.values == accB.values);
        CHECK(nnue::evaluate(*net, accA, a) == nnue::evaluate(*net, accB, b));
    }
}

TEST_CASE("the output bucket follows the piece count") {
    // Zero output weights leave only the bucket's bias, so the score names the bucket.
    auto net = randomNetwork(3);
    for (auto& row : net->outputWeights)
        row.fill(0);
    constexpr int16_t BIAS_STEP = nnue::QA * nnue::QB / 40; // 10 cp per bucket
    for (int b = 0; b < nnue::OUTPUT_BUCKETS; ++b)
        net->outputBias[b] = static_cast<int16_t>((b + 1) * BIAS_STEP);

    const std::pair<const char*, int> cases[] = {
        {"8/8/8/4k3/8/8/8/3QK3 w - - 0 1", 0},                                       // 3 pieces
        {"8/8/8/3pk3/8/2N5/8/3QK3 w - - 0 1", 0},                                    // 5
        {"8/8/8/3pk3/8/2NB4/8/3QK3 w - - 0 1", 1},                                   // 6
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 7}, // 32
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 7},             // 32
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKB1R w KQkq - 0 1", 7},             // 31
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/R1BQKB1R w KQkq - 0 1", 7},             // 30
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/R1BQK2R w KQkq - 0 1", 6},              // 29
    };
    for (const auto& [fen, bucket] : cases) {
        const Board       board(fen);
        nnue::Accumulator acc;
        nnue::refresh(*net, board, acc);
        CHECK(nnue::evaluate(*net, acc, board) == (bucket + 1) * 10);
    }
}

TEST_CASE("evaluate matches the reference formula with full-range output weights") {
    // Accumulators straddle both clamp edges and the output weights span the whole range a
    // trained net may use, so the vectorised activatedDot is checked where its int16
    // product is tightest. The reference is the plain formula in 64-bit arithmetic.
    auto                               net = randomNetwork(4);
    std::mt19937_64                    rng(11);
    std::uniform_int_distribution<int> weight(-MAX_OUTPUT_WEIGHT, MAX_OUTPUT_WEIGHT);
    std::uniform_int_distribution<int> lane(ACC_LOW, ACC_HIGH);
    for (auto& row : net->outputWeights)
        for (auto& v : row)
            v = static_cast<int16_t>(weight(rng));

    const Board board("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4");
    const int   bucket = nnue::OUTPUT_BUCKETS - 1; // all 32 pieces on the board
    for (int trial = 0; trial < 50; ++trial) {
        nnue::Accumulator acc;
        for (auto& side : acc.values)
            for (auto& v : side)
                v = static_cast<int16_t>(lane(rng));

        const auto& ours   = acc.values[static_cast<int>(board.sideToMove())];
        const auto& theirs = acc.values[static_cast<int>(~board.sideToMove())];
        const auto& w      = net->outputWeights[bucket];
        const auto  act    = [](int16_t x) { return std::clamp<int64_t>(x, 0, nnue::QA); };
        int64_t     sum    = 0;
        for (int i = 0; i < nnue::HIDDEN; ++i)
            sum += act(ours[i]) * act(ours[i]) * w[i] +
                   act(theirs[i]) * act(theirs[i]) * w[nnue::HIDDEN + i];
        const int64_t raw =
            (sum / nnue::QA + net->outputBias[bucket]) * nnue::SCALE / (nnue::QA * nnue::QB);
        const int64_t expected =
            std::clamp<int64_t>(raw, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
        REQUIRE(nnue::evaluate(*net, acc, board) == expected);
    }
}

TEST_CASE("a network with an output weight beyond the int16 product range is rejected") {
    auto       net   = randomNetwork(5);
    const auto bytes = [&net] {
        return nnue::networkFromBytes(reinterpret_cast<const unsigned char*>(net.get()),
                                      sizeof(nnue::Network));
    };
    net->outputWeights[3][5] = MAX_OUTPUT_WEIGHT;
    net->outputWeights[6][9] = -MAX_OUTPUT_WEIGHT;
    CHECK(bytes() != nullptr);
    net->outputWeights[3][5] = MAX_OUTPUT_WEIGHT + 1;
    CHECK(bytes() == nullptr);
    net->outputWeights[3][5] = 0;
    net->outputWeights[6][9] = -MAX_OUTPUT_WEIGHT - 1;
    CHECK(bytes() == nullptr);
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
        return nnue::evaluate(*net, acc, board);
    };
    const Value start = eval("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(std::abs(start) < 100);
    CHECK(eval("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") > 800);
    CHECK(eval("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNB1KBNR w KQkq - 0 1") < -800);
}
