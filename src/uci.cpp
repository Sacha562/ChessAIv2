#include "uci.hpp"
#include "search.hpp"
#include "bench.hpp"
#include "perft.hpp"
#include "chess.hpp"

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace chess;

namespace engine {

namespace {

constexpr char ENGINE_NAME[]   = "ChessAIv2";
constexpr char ENGINE_AUTHOR[] = "sivuc";

class Engine {
public:
    Engine() : board_(constants::STARTPOS) {}
    ~Engine() { stopSearch(); }

    void loop();

private:
    void handlePosition(std::istringstream& is);
    void handleGo(std::istringstream& is);
    void handleSetOption(std::istringstream& is);
    void stopSearch();

    Board board_;
    std::atomic<bool> stop_{false};
    std::thread searchThread_;

    // Options (accepted now, wired up in later phases).
    int hashMb_  = 16;
    int threads_ = 1;

    // Time-management tunables, live per-search (Phase 1a step 2).
    TimeConfig timeCfg_{};
};

void Engine::stopSearch() {
    stop_.store(true);
    if (searchThread_.joinable())
        searchThread_.join();
    stop_.store(false);
}

void Engine::handlePosition(std::istringstream& is) {
    std::string token;
    is >> token;

    std::string fen;
    if (token == "startpos") {
        fen = constants::STARTPOS;
        is >> token;                      // consume "moves" if present
    } else if (token == "fen") {
        while (is >> token && token != "moves")
            fen += token + ' ';
    } else {
        return;                           // malformed
    }

    board_ = Board(fen);

    if (token == "moves") {
        std::string mv;
        while (is >> mv) {
            Move m = uci::uciToMove(board_, mv);
            if (m == Move(Move::NO_MOVE)) break;   // stop on a malformed/illegal token
            board_.makeMove(m);
        }
    }
}

void Engine::handleGo(std::istringstream& is) {
    Limits limits;
    std::string token;
    while (is >> token) {
        if      (token == "depth")     is >> limits.depth;
        else if (token == "movetime")  is >> limits.movetime;
        else if (token == "nodes")     is >> limits.nodes;
        else if (token == "wtime")     is >> limits.wtime;
        else if (token == "btime")     is >> limits.btime;
        else if (token == "winc")      is >> limits.winc;
        else if (token == "binc")      is >> limits.binc;
        else if (token == "movestogo") is >> limits.movestogo;
        else if (token == "infinite")  limits.infinite = true;
    }

    stop_.store(false);
    Board snapshot = board_;              // search on a copy
    const TimeConfig tc = timeCfg_;
    searchThread_ = std::thread([this, snapshot, limits, tc]() mutable {
        Searcher searcher(stop_, tc);
        searcher.think(snapshot, limits, /*printBest=*/true, /*printInfo=*/true);
    });
}

void Engine::handleSetOption(std::istringstream& is) {
    std::string token, name, value;
    is >> token;                          // "name"
    while (is >> token && token != "value") {
        if (!name.empty()) name += ' ';
        name += token;
    }
    if (token == "value")
        std::getline(is >> std::ws, value);

    try {
        if (name == "Hash" && !value.empty())    hashMb_  = std::stoi(value);
        else if (name == "Threads" && !value.empty()) threads_ = std::stoi(value);
        else if (name == "TimeSoftPermille" && !value.empty()) timeCfg_.softPermille = std::stoi(value);
        else if (name == "TimeHardPermille" && !value.empty()) timeCfg_.hardPermille = std::stoi(value);
    } catch (...) { /* ignore malformed values */ }
}

void Engine::loop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string token;
        is >> token;

        if (token == "uci") {
            std::cout << "id name " << ENGINE_NAME << '\n'
                      << "id author " << ENGINE_AUTHOR << '\n'
                      << "option name Hash type spin default 16 min 1 max 1048576\n"
                      << "option name Threads type spin default 1 min 1 max 1024\n"
                      << "option name TimeSoftPermille type spin default 600 min 1 max 100000\n"
                      << "option name TimeHardPermille type spin default 2400 min 1 max 100000\n"
                      << "uciok" << std::endl;
        } else if (token == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (token == "ucinewgame") {
            stopSearch();
            board_ = Board(constants::STARTPOS);
        } else if (token == "position") {
            stopSearch();
            handlePosition(is);
        } else if (token == "go") {
            stopSearch();
            handleGo(is);
        } else if (token == "stop") {
            stopSearch();
        } else if (token == "setoption") {
            handleSetOption(is);
        } else if (token == "quit") {
            stopSearch();
            break;
        } else if (token == "d") {
            std::cout << board_ << std::endl;
        } else if (token == "perft") {
            std::string arg;
            is >> arg;
            if (arg == "test") {
                Perft::test();
            } else {
                int d = 1;
                if (!arg.empty()) std::istringstream(arg) >> d;   // no-throw parse
                Perft::divide(board_, d);
            }
        } else if (token == "bench") {
            int d = 0;
            is >> d;
            Bench::run(d);
        }
        // Unknown tokens are ignored per the UCI spec.
    }
}

} // namespace

void run_uci() {
    Engine engine;
    engine.loop();
}

} // namespace engine
