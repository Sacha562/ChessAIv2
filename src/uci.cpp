#include "uci.hpp"
#include "search.hpp"
#include "tt.hpp"
#include "bench.hpp"
#include "perft.hpp"
#include "chess.hpp"

#include <algorithm>
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

    Board             board_;
    std::atomic<bool> stop_{false};
    std::thread       searchThread_;

    // Shared transposition table (persists across moves); sized by the Hash option.
    TranspositionTable tt_{16};

    // Options.
    int hashMb_  = 16;
    int threads_ = 1; // accepted now, wired to Lazy SMP in Phase 1d.

    // Self-play-tunable knobs (time management, eval tempo, q-search delta),
    // copied into each Searcher so they can be A/B-tuned live via UCI options.
    Tunables tunables_{};
};

void Engine::stopSearch() {
    stop_.store(true);
    if (searchThread_.joinable()) searchThread_.join();
    stop_.store(false);
}

void Engine::handlePosition(std::istringstream& is) {
    std::string token;
    is >> token;

    std::string fen;
    if (token == "startpos") {
        fen = constants::STARTPOS;
        is >> token; // consume "moves" if present
    } else if (token == "fen") {
        while (is >> token && token != "moves")
            fen += token + ' ';
    } else {
        return; // malformed
    }

    board_ = Board(fen);

    if (token == "moves") {
        std::string mv;
        while (is >> mv) {
            Move m = uci::uciToMove(board_, mv);
            if (m == Move(Move::NO_MOVE)) break; // stop on a malformed/illegal token
            board_.makeMove(m);
        }
    }
}

void Engine::handleGo(std::istringstream& is) {
    Limits      limits;
    std::string token;
    while (is >> token) {
        if (token == "depth")
            is >> limits.depth;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "wtime")
            is >> limits.wtime;
        else if (token == "btime")
            is >> limits.btime;
        else if (token == "winc")
            is >> limits.winc;
        else if (token == "binc")
            is >> limits.binc;
        else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "infinite")
            limits.infinite = true;
    }

    stop_.store(false);
    Board          snapshot = board_; // search on a copy
    const Tunables tp       = tunables_;
    searchThread_           = std::thread([this, snapshot, limits, tp]() mutable {
        Searcher searcher(stop_, tt_, tp);
        searcher.think(snapshot, limits, /*printBest=*/true, /*printInfo=*/true);
    });
}

void Engine::handleSetOption(std::istringstream& is) {
    std::string token, name, value;
    is >> token; // "name"
    while (is >> token && token != "value") {
        if (!name.empty()) name += ' ';
        name += token;
    }
    if (token == "value") std::getline(is >> std::ws, value);

    try {
        if (name == "Hash" && !value.empty()) {
            hashMb_ = std::stoi(value);
            stopSearch(); // no live search may hold the table
            tt_.resize(static_cast<size_t>(hashMb_));
        } else if (name == "Threads" && !value.empty()) {
            threads_ = std::stoi(value);
        }
        // Tunable spin options: clamp each to its advertised UCI min/max, so an
        // out-of-range value (from a GUI or an SPSA tuner sending raw spins) can never
        // reach the search as an out-of-bounds int (e.g. an overflowing `tempo` in the
        // eval sum). Keep these ranges in sync with the `option` strings in `loop`.
        else if (name == "TimeSoftPermille" && !value.empty()) {
            tunables_.softPermille = std::clamp(std::stoi(value), 1, 100000);
        } else if (name == "TimeHardPermille" && !value.empty()) {
            tunables_.hardPermille = std::clamp(std::stoi(value), 1, 100000);
        } else if (name == "AssumedMovestogo" && !value.empty()) {
            tunables_.assumedMovestogo = std::clamp(std::stoi(value), 1, 300);
        } else if (name == "Tempo" && !value.empty()) {
            tunables_.tempo = std::clamp(std::stoi(value), 0, 100);
        } else if (name == "DeltaMargin" && !value.empty()) {
            tunables_.deltaMargin = std::clamp(std::stoi(value), 0, 1000);
        } else if (name == "EndgamePieces" && !value.empty()) {
            tunables_.endgamePieces = std::clamp(std::stoi(value), 0, 32);
        }
        // Ordering-heuristic toggles (0/1) — for A/B-isolating each signal's Elo.
        else if (name == "UseKillers" && !value.empty()) {
            tunables_.useKillers = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseHistory" && !value.empty()) {
            tunables_.useHistory = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseCountermove" && !value.empty()) {
            tunables_.useCountermove = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseContHist" && !value.empty()) {
            tunables_.useContHist = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseIIR" && !value.empty()) {
            tunables_.useIir = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseNMP" && !value.empty()) {
            tunables_.useNmp = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseRFP" && !value.empty()) {
            tunables_.useRfp = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseFutility" && !value.empty()) {
            tunables_.useFutility = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseLMP" && !value.empty()) {
            tunables_.useLmp = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseLMR" && !value.empty()) {
            tunables_.useLmr = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseCheckExt" && !value.empty()) {
            tunables_.useCheckExt = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseSingular" && !value.empty()) {
            tunables_.useSingular = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "UseAspiration" && !value.empty()) {
            tunables_.useAspiration = std::clamp(std::stoi(value), 0, 1) != 0;
        } else if (name == "AspirationDelta" && !value.empty()) {
            tunables_.aspirationDelta = std::clamp(std::stoi(value), 1, 500);
        }
        // Phase 1b forward-pruning margins (SPSA-tunable). LMR base/divisor are x100.
        else if (name == "LmrBase" && !value.empty()) {
            tunables_.lmrBase = std::clamp(std::stoi(value), 0, 200);
        } else if (name == "LmrDivisor" && !value.empty()) {
            tunables_.lmrDivisor = std::clamp(std::stoi(value), 50, 500);
        } else if (name == "NmpBase" && !value.empty()) {
            tunables_.nmpBase = std::clamp(std::stoi(value), 0, 6);
        } else if (name == "NmpEvalDiv" && !value.empty()) {
            tunables_.nmpEvalDiv = std::clamp(std::stoi(value), 50, 600);
        } else if (name == "RfpMargin" && !value.empty()) {
            tunables_.rfpMargin = std::clamp(std::stoi(value), 10, 300);
        } else if (name == "FutMargin" && !value.empty()) {
            tunables_.futMargin = std::clamp(std::stoi(value), 10, 300);
        } else if (name == "FutBase" && !value.empty()) {
            tunables_.futBase = std::clamp(std::stoi(value), 0, 400);
        } else if (name == "LmpBase" && !value.empty()) {
            tunables_.lmpBase = std::clamp(std::stoi(value), 0, 12);
        }
        // Phase 1c singular-extension knobs (SPSA-tunable).
        else if (name == "SingularMinDepth" && !value.empty()) {
            tunables_.singularMinDepth = std::clamp(std::stoi(value), 4, 12);
        } else if (name == "SingularMargin" && !value.empty()) {
            tunables_.singularMargin = std::clamp(std::stoi(value), 1, 8);
        } else if (name == "SingularDoubleMargin" && !value.empty()) {
            tunables_.singularDoubleMargin = std::clamp(std::stoi(value), 0, 200);
        }
    } catch (...) { /* ignore malformed values */
    }
}

void Engine::loop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string        token;
        is >> token;

        if (token == "uci") {
            std::cout << "id name " << ENGINE_NAME << '\n'
                      << "id author " << ENGINE_AUTHOR << '\n'
                      << "option name Hash type spin default 16 min 1 max 1048576\n"
                      << "option name Threads type spin default 1 min 1 max 1024\n"
                      << "option name TimeSoftPermille type spin default 600 min 1 max 100000\n"
                      << "option name TimeHardPermille type spin default 2400 min 1 max 100000\n"
                      << "option name AssumedMovestogo type spin default 30 min 1 max 300\n"
                      << "option name Tempo type spin default 9 min 0 max 100\n"
                      << "option name DeltaMargin type spin default 203 min 0 max 1000\n"
                      << "option name EndgamePieces type spin default 7 min 0 max 32\n"
                      << "option name UseKillers type spin default 1 min 0 max 1\n"
                      << "option name UseHistory type spin default 1 min 0 max 1\n"
                      << "option name UseCountermove type spin default 1 min 0 max 1\n"
                      << "option name UseContHist type spin default 1 min 0 max 1\n"
                      << "option name UseIIR type spin default 1 min 0 max 1\n"
                      << "option name UseNMP type spin default 1 min 0 max 1\n"
                      << "option name UseRFP type spin default 1 min 0 max 1\n"
                      << "option name UseFutility type spin default 1 min 0 max 1\n"
                      << "option name UseLMP type spin default 1 min 0 max 1\n"
                      << "option name UseLMR type spin default 1 min 0 max 1\n"
                      << "option name UseCheckExt type spin default 1 min 0 max 1\n"
                      << "option name UseSingular type spin default 1 min 0 max 1\n"
                      << "option name UseAspiration type spin default 1 min 0 max 1\n"
                      << "option name AspirationDelta type spin default 15 min 1 max 500\n"
                      << "option name LmrBase type spin default 68 min 0 max 200\n"
                      << "option name LmrDivisor type spin default 162 min 50 max 500\n"
                      << "option name NmpBase type spin default 1 min 0 max 6\n"
                      << "option name NmpEvalDiv type spin default 176 min 50 max 600\n"
                      << "option name RfpMargin type spin default 68 min 10 max 300\n"
                      << "option name FutMargin type spin default 86 min 10 max 300\n"
                      << "option name FutBase type spin default 77 min 0 max 400\n"
                      << "option name LmpBase type spin default 4 min 0 max 12\n"
                      << "option name SingularMinDepth type spin default 7 min 4 max 12\n"
                      << "option name SingularMargin type spin default 1 min 1 max 8\n"
                      << "option name SingularDoubleMargin type spin default 46 min 0 max 200\n"
                      << "uciok" << std::endl;
        } else if (token == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (token == "ucinewgame") {
            stopSearch();
            tt_.clear();
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
                if (!arg.empty()) std::istringstream(arg) >> d; // no-throw parse
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
