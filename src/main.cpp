#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <string>

#include "uci.hpp"
#include "bench.hpp"
#include "datagen.hpp"
#include "perft.hpp"
#include "chess.hpp"

namespace {
constexpr int DATAGEN_MIN_ARGS = 5; // chessai datagen <out-prefix> <threads> <positions>

// A fresh, unpredictable seed when none is given, so two runs never replay the same
// games into the same files. runDatagen prints it, so a run can still be reproduced.
uint64_t randomSeed() {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) | device();
}

// chessai datagen <out-prefix> <threads> <positions> [seed] [soft-nodes]
std::optional<engine::DatagenConfig> parseDatagen(int argc, char** argv) {
    if (argc < DATAGEN_MIN_ARGS) return std::nullopt;
    engine::DatagenConfig config;
    config.outPrefix = argv[2];
    config.threads   = std::atoi(argv[3]);
    config.positions = std::strtoull(argv[4], nullptr, 10);
    config.seed      = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : randomSeed();
    if (argc > 6) config.softNodes = std::strtoll(argv[6], nullptr, 10);
    if (config.threads < 1 || config.positions == 0 || config.softNodes < 1) return std::nullopt;
    return config;
}
} // namespace

// Entry point. Supports:
//   chessai                -> UCI loop (default)
//   chessai bench [depth]  -> deterministic benchmark (OpenBench signature)
//   chessai perft test     -> run the perft correctness suite (exit 1 on failure)
//   chessai datagen <out-prefix> <threads> <positions> [seed] [soft-nodes]
//                          -> self-play NNUE training data (viriformat)
int main(int argc, char** argv) {
    const std::string cmd = (argc > 1) ? argv[1] : "";

    if (cmd == "bench") {
        const int depth = (argc > 2) ? std::atoi(argv[2]) : 0;
        engine::Bench::run(depth);
        return 0;
    }

    if (cmd == "perft" && argc > 2 && std::string(argv[2]) == "test") {
        return engine::Perft::test() ? 0 : 1;
    }

    if (cmd == "datagen") {
        const auto config = parseDatagen(argc, argv);
        if (config) return engine::runDatagen(*config);
        std::cerr << "usage: chessai datagen <out-prefix> <threads> <positions> [seed] "
                     "[soft-nodes]"
                  << std::endl;
        return 1;
    }

    engine::run_uci();
    return 0;
}
