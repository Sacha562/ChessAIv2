#include "nnue.hpp"
#include "chess.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>

using namespace chess;

// The default network, linked into the binary: the build passes the net file's absolute
// path as the EVALFILE string macro and the assembler copies its bytes into read-only
// data with `.incbin` (the portable `#embed` is missing from the MinGW GCC 13 used for
// Windows releases). The section switch is spelled per object format because the asm
// text must be one literal. MSVC has no GNU-style top-level asm, so it embeds nothing.
#if defined(EVALFILE) && defined(__GNUC__)
#define CHESSAI_HAS_EMBEDDED_NET 1
#if defined(_WIN32)
__asm__(".section .rdata,\"dr\"\n"
        ".balign 64\n"
        ".globl chessai_net_begin\n"
        "chessai_net_begin:\n"
        ".incbin \"" EVALFILE "\"\n"
        ".globl chessai_net_end\n"
        "chessai_net_end:\n"
        ".text\n");
#else
__asm__(".section .rodata\n"
        ".balign 64\n"
        ".globl chessai_net_begin\n"
        "chessai_net_begin:\n"
        ".incbin \"" EVALFILE "\"\n"
        ".globl chessai_net_end\n"
        "chessai_net_end:\n"
        ".text\n");
#endif
extern "C" const unsigned char chessai_net_begin[];
extern "C" const unsigned char chessai_net_end[];
#endif

namespace engine {
namespace nnue {

namespace {
constexpr int NUM_SQUARES  = 64;
constexpr int THEIR_OFFSET = 6 * NUM_SQUARES; // their pieces follow our six piece blocks
constexpr int FLIP_RANKS   = 56;              // sq ^ 56 mirrors a1 <-> a8 for Black's view

// Bytes of real data in a bullet quantised.bin (the file pads this up to 64).
constexpr std::streamsize NETWORK_BYTES =
    static_cast<std::streamsize>(sizeof(int16_t)) * (INPUTS * HIDDEN + HIDDEN + 2 * HIDDEN + 1);

static_assert(offsetof(Network, featureBias) == sizeof(int16_t) * INPUTS * HIDDEN);
static_assert(offsetof(Network, outputWeights) == sizeof(int16_t) * (INPUTS + 1) * HIDDEN);
static_assert(offsetof(Network, outputBias) == sizeof(int16_t) * (INPUTS + 3) * HIDDEN);

// One input feature before it is indexed for a perspective.
struct Feature {
    Color     color;
    PieceType type;
    Square    square;
};

// The features a move switches on and off: at most two of each (castling moves king
// and rook; a capture or en passant also removes the victim).
struct Delta {
    std::array<Feature, 2> added{};
    std::array<Feature, 2> removed{};
    int                    nAdded   = 0;
    int                    nRemoved = 0;
};

// Bullet's Chess768 layout, seen from `perspective`: own pieces first, then theirs, each
// as type * 64 + square, with the board mirrored vertically for Black.
int featureIndex(Color perspective, const Feature& f) {
    const int side = f.color == perspective ? 0 : THEIR_OFFSET;
    const int sq   = perspective == Color::WHITE ? f.square.index() : f.square.index() ^ FLIP_RANKS;
    return side + static_cast<int>(f.type) * NUM_SQUARES + sq;
}

Delta moveDelta(const Board& board, Move move) {
    Delta           d;
    const Color     us    = board.sideToMove();
    const Square    from  = move.from();
    const Square    to    = move.to();
    const PieceType moved = board.at<PieceType>(from);

    // The library encodes castling as "king takes own rook": `to` is the rook's square.
    if (move.typeOf() == Move::CASTLING) {
        const bool kingSide = to > from;
        d.removed  = {Feature{us, PieceType::KING, from}, Feature{us, PieceType::ROOK, to}};
        d.added    = {Feature{us, PieceType::KING, Square::castling_king_square(kingSide, us)},
                      Feature{us, PieceType::ROOK, Square::castling_rook_square(kingSide, us)}};
        d.nRemoved = 2;
        d.nAdded   = 2;
        return d;
    }

    const PieceType placed  = move.typeOf() == Move::PROMOTION ? move.promotionType() : moved;
    d.removed[d.nRemoved++] = Feature{us, moved, from};
    d.added[d.nAdded++]     = Feature{us, placed, to};

    if (move.typeOf() == Move::ENPASSANT)
        d.removed[d.nRemoved++] = Feature{~us, PieceType::PAWN, to.ep_square()};
    else if (board.at(to) != Piece::NONE)
        d.removed[d.nRemoved++] = Feature{~us, board.at<PieceType>(to), to};
    return d;
}

// child = parent + sum(added columns) - sum(removed columns). Templated on the counts so
// each of the three move shapes compiles to one vectorised pass over the accumulator.
template <int ADDS, int SUBS>
void applyDelta(const Network& net, const std::array<int16_t, HIDDEN>& parent,
                std::array<int16_t, HIDDEN>& child, const std::array<int, 2>& add,
                const std::array<int, 2>& sub) {
    for (int i = 0; i < HIDDEN; ++i) {
        int16_t v = parent[i];
        for (int a = 0; a < ADDS; ++a)
            v = static_cast<int16_t>(v + net.featureWeights[add[a]][i]);
        for (int s = 0; s < SUBS; ++s)
            v = static_cast<int16_t>(v - net.featureWeights[sub[s]][i]);
        child[i] = v;
    }
}

// Square Clipped ReLU: clamp to [0, QA], then square (so the result is in QA^2 units).
int32_t screlu(int16_t x) {
    const int32_t v = std::clamp<int32_t>(x, 0, QA);
    return v * v;
}

// One perspective's share of the output: sum of SCReLU(acc[i]) * weights[i]. Kept as a
// single-stream reduction because MinGW GCC 13 (the Windows release compiler) leaves the
// two-perspective form of this loop scalar, ~3x slower; this form vectorises everywhere.
int32_t activatedDot(const std::array<int16_t, HIDDEN>& acc, const int16_t* weights) {
    int32_t sum = 0;
    for (int i = 0; i < HIDDEN; ++i)
        sum += screlu(acc[i]) * weights[i];
    return sum;
}
} // namespace

std::unique_ptr<Network> loadNetwork(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < NETWORK_BYTES) return nullptr;
    in.seekg(0);
    auto net = std::make_unique<Network>();
    in.read(reinterpret_cast<char*>(net.get()), NETWORK_BYTES);
    return in ? std::move(net) : nullptr;
}

std::unique_ptr<Network> networkFromBytes(const unsigned char* data, std::size_t size) {
    if (data == nullptr || size < static_cast<std::size_t>(NETWORK_BYTES)) return nullptr;
    auto net = std::make_unique<Network>();
    std::memcpy(net.get(), data, static_cast<std::size_t>(NETWORK_BYTES));
    return net;
}

const Network* embeddedNetwork() {
#if defined(CHESSAI_HAS_EMBEDDED_NET)
    // Copied once into an aligned Network on first use (a thread-safe static), then shared
    // read-only by every search.
    static const std::unique_ptr<Network> net = networkFromBytes(
        chessai_net_begin, static_cast<std::size_t>(chessai_net_end - chessai_net_begin));
    return net.get();
#else
    return nullptr;
#endif
}

void refresh(const Network& net, const Board& board, Accumulator& out) {
    for (const Color perspective : {Color::WHITE, Color::BLACK}) {
        auto& values      = out.values[static_cast<int>(perspective)];
        values            = net.featureBias;
        Bitboard occupied = board.occ();
        while (occupied) {
            const Square  sq    = Square(static_cast<int>(occupied.pop()));
            const Piece   piece = board.at(sq);
            const Feature f{piece.color(), piece.type(), sq};
            const auto&   column = net.featureWeights[featureIndex(perspective, f)];
            for (int i = 0; i < HIDDEN; ++i)
                values[i] = static_cast<int16_t>(values[i] + column[i]);
        }
    }
}

void update(const Network& net, const Board& board, Move move, const Accumulator& parent,
            Accumulator& child) {
    const Delta d = moveDelta(board, move);
    for (const Color perspective : {Color::WHITE, Color::BLACK}) {
        const int          p = static_cast<int>(perspective);
        std::array<int, 2> add{};
        std::array<int, 2> sub{};
        for (int i = 0; i < d.nAdded; ++i)
            add[i] = featureIndex(perspective, d.added[i]);
        for (int i = 0; i < d.nRemoved; ++i)
            sub[i] = featureIndex(perspective, d.removed[i]);

        if (d.nAdded == 2)
            applyDelta<2, 2>(net, parent.values[p], child.values[p], add, sub);
        else if (d.nRemoved == 2)
            applyDelta<1, 2>(net, parent.values[p], child.values[p], add, sub);
        else
            applyDelta<1, 1>(net, parent.values[p], child.values[p], add, sub);
    }
}

Value evaluate(const Network& net, const Accumulator& acc, Color stm) {
    const auto& ours   = acc.values[static_cast<int>(stm)];
    const auto& theirs = acc.values[static_cast<int>(~stm)];

    // int32 is the reference implementation's accumulator width; real activations stay
    // far below its range (the theoretical worst case does not occur in play).
    const int32_t sum = activatedDot(ours, net.outputWeights.data()) +
                        activatedDot(theirs, net.outputWeights.data() + HIDDEN);

    // QA^2 * QB units -> QA * QB, add the bias, then scale to centipawns.
    const int32_t output = (sum / QA + net.outputBias) * SCALE / (QA * QB);
    return std::clamp<Value>(output, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}

} // namespace nnue
} // namespace engine
