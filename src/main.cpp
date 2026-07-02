#include <cstdlib>
#include <string>

#include "uci.hpp"
#include "bench.hpp"
#include "perft.hpp"
#include "chess.hpp"

// Entry point. Supports:
//   chessai                -> UCI loop (default)
//   chessai bench [depth]  -> deterministic benchmark (OpenBench signature)
//   chessai perft test     -> run the perft correctness suite (exit 1 on failure)
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

    engine::run_uci();
    return 0;
}
