// Unit tests for the hand-crafted evaluation (src/eval.*). The eval is
// side-to-move-relative and tapered; these cases pin the invariants that a PSQT +
// tapered eval must hold regardless of the exact (tunable) weights:
//   - colour symmetry: a position and its vertical-mirror/colour-swap score equal
//     from each mover's own perspective;
//   - a symmetric position (startpos) is a dead draw (== tempo);
//   - material dominates (a queen up is a large plus);
//   - the PSQT prefers a centralised piece to a cornered one.
// tempo = 0 throughout so the material/positional signal is read cleanly.
#include "doctest.h"

#include <cctype>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "chess.hpp"
#include "eval.hpp"

using namespace engine;
using namespace chess;

namespace {

// Vertically mirror + colour-swap a FEN and flip the side to move: the "same"
// position with the colours exchanged. Castling and en passant are dropped (set to
// "-") so the helper stays trivially correct — the eval ignores them anyway.
std::string mirrorFen(const std::string& fen) {
    std::istringstream iss(fen);
    std::string        board, stm;
    iss >> board >> stm;

    std::string              rank;
    std::vector<std::string> ranks;
    std::istringstream       bs(board);
    while (std::getline(bs, rank, '/'))
        ranks.push_back(rank);

    std::string flipped;
    for (auto it = ranks.rbegin(); it != ranks.rend(); ++it) {
        for (char c : *it) {
            flipped += std::isupper(static_cast<unsigned char>(c))
                           ? static_cast<char>(std::tolower(c))
                           : static_cast<char>(std::toupper(c));
        }
        if (std::next(it) != ranks.rend()) flipped += '/';
    }

    const std::string side = (stm == "w") ? "b" : "w";
    return flipped + ' ' + side + " - - 0 1";
}

Value eval(const std::string& fen) {
    return evaluate(Board(fen), 0);
}

} // namespace

TEST_CASE("evaluation is colour-symmetric") {
    const char* fens[] = {
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w - - 0 1", // an open game
        "4k3/8/8/3N4/8/2b5/8/4K3 b - - 0 1",                             // lopsided minors
        "8/2p2p2/1p4p1/p6p/P6P/1P4P1/2P2P2/8 w - - 0 1",                 // pawns only
    };
    for (const char* fen : fens) {
        CHECK(eval(fen) == eval(mirrorFen(fen)));
    }
}

TEST_CASE("a symmetric position is level") {
    // startpos: mirror image, so the side-relative eval is exactly the tempo (0 here).
    CHECK(eval("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1") == 0);
}

TEST_CASE("a queen up is a large advantage") {
    // White has an extra queen; the mover (White) should be up close to a queen.
    const Value v = eval("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    CHECK(v > 700);
}

TEST_CASE("PSQT prefers a centralised knight to a cornered one") {
    // Same material (White up a knight, kings symmetric), knight central vs in the
    // corner — the PSQT must reward the centre.
    const Value central = eval("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    const Value corner  = eval("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(central > corner);
}
