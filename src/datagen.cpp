#include "datagen.hpp"
#include "search.hpp"
#include "tt.hpp"
#include "chess.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace chess;

namespace engine {

using viriformat::Outcome;

namespace {
constexpr std::size_t HASH_MB          = 16; // per-worker TT, cleared every game
constexpr int64_t     HARD_NODE_FACTOR = 20; // hard node cap = softNodes * this (safety net)

// Openings: this many random legal plies from startpos, plus 0 or 1 more so either side
// can be first to move in the scored game; a start a search already calls decided is
// rejected (it would teach nothing but one-sided positions).
constexpr int   OPENING_PLIES       = 8;
constexpr int   OPENING_EXTRA_PLIES = 2;
constexpr Value OPENING_MAX_SCORE   = 1000; // cp

// Golden-ratio stride: spreads the per-worker seeds so no two workers share a stream.
constexpr uint64_t SEED_STRIDE = 0x9E3779B97F4A7C15ull;

constexpr auto PROGRESS_POLL     = std::chrono::milliseconds(200);
constexpr auto PROGRESS_INTERVAL = std::chrono::seconds(10);

// Counters shared by every worker and the reporting (main) thread.
struct Progress {
    explicit Progress(uint64_t goal) : target(goal) {}

    const uint64_t        target;
    std::atomic<uint64_t> positions{0};
    std::atomic<uint64_t> games{0};
    std::atomic<int>      running{0}; // workers that have not finished yet

    bool reached() const { return positions.load(std::memory_order_relaxed) >= target; }
};

struct SearchResult {
    Move  move;
    Value score; // side-to-move cp
};

std::string outputPath(const DatagenConfig& config, int index) {
    return config.outPrefix + "-" + std::to_string(index) + ".vf";
}

// Game ply of `board`, counted from the standard start position.
int gamePly(const Board& board) {
    const int black = board.sideToMove() == Color::BLACK ? 1 : 0;
    return (static_cast<int>(board.fullMoveNumber()) - 1) * 2 + black;
}

// The library reports a finished game from the side to move's view: only LOSE (mated)
// or DRAW can occur, since the side that just moved is the one that could have won.
Outcome outcomeOf(GameResult result, Color stm) {
    if (result != GameResult::LOSE) return Outcome::Draw;
    return stm == Color::WHITE ? Outcome::BlackWin : Outcome::WhiteWin;
}

// One generator thread's state: its own TT, searcher, RNG, and output file, so workers
// share nothing but the Progress counters.
class Worker {
public:
    Worker(const DatagenConfig& config, int index, Progress& progress)
        : progress_(progress), out_(outputPath(config, index), std::ios::binary | std::ios::app),
          rng_(config.seed ^ (SEED_STRIDE * static_cast<uint64_t>(index + 1))) {
        limits_.softNodes = config.softNodes;
        limits_.nodes     = config.softNodes * HARD_NODE_FACTOR;
    }

    bool isOpen() const { return out_.is_open(); }

    // Play and write games until the shared position target is reached.
    void run() {
        while (!progress_.reached()) {
            tt_.clear();
            const Board start = randomOpening();
            game_.reset(start);
            const Outcome outcome = playGame(start);
            game_.write(out_, outcome);
            out_.flush();
            progress_.positions.fetch_add(game_.positions(), std::memory_order_relaxed);
            progress_.games.fetch_add(1, std::memory_order_relaxed);
        }
        progress_.running.fetch_sub(1, std::memory_order_release);
    }

private:
    // A fresh Searcher per move, exactly as the UCI `go` handler builds one, so the data
    // reflects the engine as it plays. Emplaced in place: no allocation per move.
    SearchResult searchPosition(const Board& board) {
        searcher_.emplace(state_, tt_);
        const Move move =
            searcher_->think(board, limits_, /*printBest=*/false, /*printInfo=*/false);
        return {move, searcher_->score()};
    }

    // Play `plies` uniformly random legal moves; false if the game ends on the way.
    bool playRandomPlies(Board& board, int plies) {
        for (int i = 0; i < plies; ++i) {
            Movelist moves;
            movegen::legalmoves(moves, board);
            if (moves.empty()) return false;
            board.makeMove(moves[static_cast<int>(rng_() % moves.size())]);
        }
        return true;
    }

    Board randomOpening() {
        for (;;) {
            Board      board;
            const auto plies = OPENING_PLIES + static_cast<int>(rng_() % OPENING_EXTRA_PLIES);
            if (!playRandomPlies(board, plies)) continue;
            if (board.isGameOver().second != GameResult::NONE) continue;
            if (std::abs(searchPosition(board).score) <= OPENING_MAX_SCORE) return board;
        }
    }

    // Play `board` (a deliberate copy: the game is made on it) to its end or an
    // adjudication, recording every searched position into game_.
    Outcome playGame(Board board) {
        Adjudicator adjudicator;
        for (;;) {
            const GameResult result = board.isGameOver().second;
            if (result != GameResult::NONE) return outcomeOf(result, board.sideToMove());

            const SearchResult searched = searchPosition(board);
            const Value        white =
                board.sideToMove() == Color::WHITE ? searched.score : -searched.score;
            game_.addMove(searched.move, white);

            if (const auto called = adjudicator.update(white, gamePly(board))) return *called;
            board.makeMove(searched.move);
        }
    }

    Progress&                progress_;
    std::ofstream            out_;
    std::mt19937_64          rng_;
    std::atomic<SearchState> state_{
        SearchState::Running}; // never stopped: node limits end each search
    TranspositionTable      tt_{HASH_MB};
    std::optional<Searcher> searcher_;
    Limits                  limits_;
    viriformat::Game        game_;
};

void printProgress(const Progress& progress, std::chrono::steady_clock::time_point start) {
    using namespace std::chrono;
    const auto     ms        = duration_cast<milliseconds>(steady_clock::now() - start).count();
    const uint64_t positions = progress.positions.load(std::memory_order_relaxed);
    const uint64_t rate      = ms > 0 ? positions * 1000ull / static_cast<uint64_t>(ms) : 0;
    std::cout << "datagen: " << progress.games.load(std::memory_order_relaxed) << " games, "
              << positions << " positions, " << rate << " pos/s, " << ms / 1000 << " s"
              << std::endl;
}

// Block until every worker has finished, printing progress every PROGRESS_INTERVAL.
void reportUntilDone(const Progress& progress, std::chrono::steady_clock::time_point start) {
    auto lastReport = std::chrono::steady_clock::now();
    while (progress.running.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(PROGRESS_POLL);
        if (std::chrono::steady_clock::now() - lastReport < PROGRESS_INTERVAL) continue;
        printProgress(progress, start);
        lastReport = std::chrono::steady_clock::now();
    }
}
} // namespace

std::optional<Outcome> Adjudicator::update(Value whiteScore, int ply) {
    whiteWinPlies_   = whiteScore >= WIN_SCORE ? whiteWinPlies_ + 1 : 0;
    blackWinPlies_   = whiteScore <= -WIN_SCORE ? blackWinPlies_ + 1 : 0;
    const bool level = ply >= DRAW_MIN_PLY && std::abs(whiteScore) <= DRAW_SCORE;
    drawPlies_       = level ? drawPlies_ + 1 : 0;

    if (whiteWinPlies_ >= WIN_PLIES) return Outcome::WhiteWin;
    if (blackWinPlies_ >= WIN_PLIES) return Outcome::BlackWin;
    if (drawPlies_ >= DRAW_PLIES) return Outcome::Draw;
    return std::nullopt;
}

int runDatagen(const DatagenConfig& config) {
    Progress progress(config.positions);

    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < config.threads; ++i) {
        workers.push_back(std::make_unique<Worker>(config, i, progress));
        if (workers.back()->isOpen()) continue;
        std::cerr << "datagen: cannot open " << outputPath(config, i) << std::endl;
        return 1;
    }

    std::cout << "datagen: " << config.threads << " threads, target " << config.positions
              << " positions, " << config.softNodes << " soft nodes, seed " << config.seed
              << ", output " << config.outPrefix << "-*.vf" << std::endl;

    const auto start = std::chrono::steady_clock::now();
    progress.running.store(config.threads, std::memory_order_relaxed);
    {
        std::vector<std::jthread> threads;
        for (const auto& worker : workers)
            threads.emplace_back([&w = *worker] { w.run(); });
        reportUntilDone(progress, start);
    } // the jthreads join here

    printProgress(progress, start);
    return 0;
}

} // namespace engine
