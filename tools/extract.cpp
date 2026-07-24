// PGN -> labeled-FEN extractor for Texel tuning (see tools/tune.cpp).
//
// Replays each game in a PGN (e.g. fastchess self-play output) and emits quiet,
// decorrelated positions labeled with the game result, one "FEN result" per line —
// exactly the dataset format the tuner consumes. Dependency-free: it uses the
// vendored Disservin library's PGN parser and SAN replay, so no Python / python-chess
// is needed.
//
// A position is emitted only if it is *quiet*: past the opening (>= --skip-plies),
// the side to move is not in check, and the move played from it is neither a capture
// nor a promotion (those are tactical, where the static eval is unreliable). The
// result label is the game outcome from White's perspective (1.0 / 0.5 / 0.0).
//
// Usage:
//   extract <pgn> [--skip-plies N] > dataset.txt      (N default 8)
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "chess.hpp"

using namespace chess;

namespace {

// Map a PGN result header to a White-POV label token, or nullptr if undecided.
const char* resultToken(const std::string& result) {
    if (result == "1-0") return "1.0";
    if (result == "0-1") return "0.0";
    if (result == "1/2-1/2") return "0.5";
    return nullptr; // '*' or missing: unusable for tuning
}

class Extractor : public pgn::Visitor {
public:
    Extractor(std::ostream& out, int skipPlies) : out_(out), skipPlies_(skipPlies) {}

    void startPgn() override {
        board_ = Board(constants::STARTPOS);
        result_.clear();
        fens_.clear();
        ply_   = 0;
        valid_ = true;
    }

    void header(std::string_view key, std::string_view value) override {
        if (key == "FEN")
            board_ = Board(std::string(value)); // book openings start from a FEN
        else if (key == "Result")
            result_ = std::string(value);
    }

    void startMoves() override {}

    void move(std::string_view san, std::string_view) override {
        if (!valid_) return;

        Move m;
        try {
            m = uci::parseSan(board_, san);
        } catch (...) {
            valid_ = false; // a move we cannot parse invalidates the rest of the game
            return;
        }
        if (m == Move(Move::NO_MOVE)) {
            valid_ = false;
            return;
        }

        // Emit the pre-move position when it is quiet and past the opening.
        if (ply_ >= skipPlies_ && !board_.inCheck() && !board_.isCapture(m) &&
            m.typeOf() != Move::PROMOTION) {
            fens_.push_back(board_.getFen());
        }
        board_.makeMove(m);
        ++ply_;
    }

    void endPgn() override {
        const char* token = resultToken(result_);
        if (token == nullptr) return;
        for (const std::string& fen : fens_)
            out_ << fen << ' ' << token << '\n';
        ++games_;
        positions_ += fens_.size();
    }

    size_t games() const { return games_; }
    size_t positions() const { return positions_; }

private:
    Board                    board_;
    std::ostream&            out_;
    int                      skipPlies_;
    int                      ply_   = 0;
    bool                     valid_ = true;
    std::string              result_;
    std::vector<std::string> fens_;
    size_t                   games_     = 0;
    size_t                   positions_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: extract <pgn> [--skip-plies N] > dataset.txt\n";
        return 1;
    }
    const std::string pgnPath   = argv[1];
    int               skipPlies = 8;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--skip-plies") skipPlies = std::stoi(argv[++i]);
    }

    std::ifstream in(pgnPath);
    if (!in) {
        std::cerr << "extract: cannot open '" << pgnPath << "'\n";
        return 1;
    }

    pgn::StreamParser parser(in);
    Extractor         vis(std::cout, skipPlies);
    parser.readGames(vis);

    std::cerr << "extract: " << vis.positions() << " positions from " << vis.games() << " games\n";
    return 0;
}
