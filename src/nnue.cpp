#include "nnue.hpp"
#include "chess.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

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
constexpr int NUM_FILES    = 8;
constexpr int HALF_FILES   = NUM_FILES / 2;
constexpr int THEIR_OFFSET = 6 * NUM_SQUARES; // their pieces follow our six piece blocks
constexpr int FLIP_RANKS   = 56;              // sq ^ 56 mirrors a1 <-> a8 for Black's view
constexpr int FLIP_FILES   = 7;               // sq ^ 7 mirrors a1 <-> h1 (king on files e-h)
constexpr int MAX_PIECES   = 32;
constexpr int MIN_PIECES   = 2; // the two kings
// Pieces per output bucket, as bullet's MaterialCount<OUTPUT_BUCKETS> divides them.
constexpr int PIECES_PER_BUCKET = (MAX_PIECES + OUTPUT_BUCKETS - 1) / OUTPUT_BUCKETS;
// activatedDot multiplies a clamped activation (at most QA) by an output weight in int16,
// so no output weight may exceed this. bullet clips them to about ±127.
constexpr int MAX_OUTPUT_WEIGHT = std::numeric_limits<int16_t>::max() / QA;

// Bytes of real data in a bullet quantised.bin (the file pads this up to 64).
constexpr int FEATURE_COLUMNS = INPUT_BUCKETS * INPUTS;

constexpr std::streamsize NETWORK_BYTES =
    static_cast<std::streamsize>(sizeof(int16_t)) *
    (FEATURE_COLUMNS * HIDDEN + HIDDEN + OUTPUT_BUCKETS * (2 * HIDDEN + 1));

static_assert(offsetof(Network, featureBias) == sizeof(int16_t) * FEATURE_COLUMNS * HIDDEN);
static_assert(offsetof(Network, outputWeights) == sizeof(int16_t) * (FEATURE_COLUMNS + 1) * HIDDEN);
static_assert(offsetof(Network, outputBias) ==
              sizeof(int16_t) * (FEATURE_COLUMNS + 1 + 2 * OUTPUT_BUCKETS) * HIDDEN);
static_assert(std::ranges::max(KING_BUCKET_LAYOUT) + 1 == INPUT_BUCKETS);

// One input feature before it is indexed for a perspective.
struct Feature {
    Color     color;
    PieceType type;
    Square    square;

    bool operator==(const Feature&) const = default;
};

// How one perspective sees the board, fixed by its own king square: the input bucket its
// king selects (as the first column of that bucket) and the XOR that maps a square into
// its view (ranks flipped for Black, files mirrored when its king is on files e-h).
struct View {
    int bucketOffset = 0;
    int squareXor    = 0;

    bool operator==(const View&) const = default;
};

// bullet's ChessBucketsMirrored, for `perspective` with its king on `king`.
View viewOf(Color perspective, Square king) {
    const int  rankFlip   = perspective == Color::WHITE ? 0 : FLIP_RANKS;
    const int  k          = king.index() ^ rankFlip;
    const int  file       = k % NUM_FILES;
    const bool mirror     = file >= HALF_FILES;
    const int  layoutFile = mirror ? NUM_FILES - 1 - file : file;
    const int  bucket     = KING_BUCKET_LAYOUT[(k / NUM_FILES) * HALF_FILES + layoutFile];
    return View{bucket * INPUTS, rankFlip ^ (mirror ? FLIP_FILES : 0)};
}

// The features a move switches on and off: at most two of each (castling moves king
// and rook; a capture or en passant also removes the victim).
struct Delta {
    std::array<Feature, 2> added{};
    std::array<Feature, 2> removed{};
    int                    nAdded   = 0;
    int                    nRemoved = 0;
};

// Bullet's Chess768 layout inside the view's bucket: own pieces first, then theirs, each
// as type * 64 + square, with the square mapped into the perspective's view.
int featureIndex(Color perspective, const View& view, const Feature& f) {
    const int side = f.color == perspective ? 0 : THEIR_OFFSET;
    const int sq   = f.square.index() ^ view.squareXor;
    return view.bucketOffset + side + static_cast<int>(f.type) * NUM_SQUARES + sq;
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

// The mover's king square once `d` is made: where an added king lands, else unchanged.
Square kingAfter(const Board& board, const Delta& d) {
    for (int i = 0; i < d.nAdded; ++i)
        if (d.added[i].type == PieceType::KING) return d.added[i].square;
    return board.kingSq(board.sideToMove());
}

bool removedBy(const Delta& d, const Feature& f) {
    return std::any_of(d.removed.begin(), d.removed.begin() + d.nRemoved,
                       [&f](const Feature& r) { return r == f; });
}

void addColumn(const Network& net, int column, std::array<int16_t, HIDDEN>& values) {
    const auto& weights = net.featureWeights[column];
    for (int i = 0; i < HIDDEN; ++i)
        values[i] = static_cast<int16_t>(values[i] + weights[i]);
}

// One perspective's accumulator built from scratch for the position `board` becomes once
// `d` is made, seen through `view`. With an empty delta this is a plain refresh; after a
// king move that changes the mover's bucket or mirroring, it replaces the incremental
// update for that perspective (every column changes).
void rebuild(const Network& net, const Board& board, const Delta& d, Color perspective,
             const View& view, std::array<int16_t, HIDDEN>& values) {
    values            = net.featureBias;
    Bitboard occupied = board.occ();
    while (occupied) {
        const Square  sq    = Square(static_cast<int>(occupied.pop()));
        const Piece   piece = board.at(sq);
        const Feature f{piece.color(), piece.type(), sq};
        if (!removedBy(d, f)) addColumn(net, featureIndex(perspective, view, f), values);
    }
    for (int i = 0; i < d.nAdded; ++i)
        addColumn(net, featureIndex(perspective, view, d.added[i]), values);
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

// The output layer a position uses: positions are grouped by piece count, 2-5 pieces in
// bucket 0 up to 30-32 in bucket 7, matching the trainer's MaterialCount buckets.
int outputBucket(const Board& board) {
    return (board.occ().count() - MIN_PIECES) / PIECES_PER_BUCKET;
}

#if defined(__AVX2__)
// Sum of the eight int32 lanes of `v`.
int32_t horizontalSum(__m256i v) {
    const __m128i four = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    const __m128i two  = _mm_add_epi32(four, _mm_unpackhi_epi64(four, four));
    const __m128i one  = _mm_add_epi32(two, _mm_shuffle_epi32(two, 1));
    return _mm_cvtsi128_si32(one);
}

// One perspective's share of the output: sum of SCReLU(acc[i]) * weights[i], in the
// "lizard" form. With v = clamp(acc[i], 0, QA), v * w fits int16 (MAX_OUTPUT_WEIGHT), so
// vpmullw forms it 16 lanes at a time and vpmaddwd multiplies by v again and adds lane
// pairs into int32: exactly v^2 * w, without the 8-lane vpmulld the plain v^2 * w form
// compiles to. Measured: evaluate ~2.3x faster, search +20% nps (nnue.hpp.md#evaluate).
int32_t activatedDot(const std::array<int16_t, HIDDEN>& acc, const int16_t* weights) {
    constexpr int LANES = static_cast<int>(sizeof(__m256i) / sizeof(int16_t));
    static_assert(HIDDEN % LANES == 0);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i clip = _mm256_set1_epi16(QA);
    __m256i       sum  = _mm256_setzero_si256();
    for (int i = 0; i < HIDDEN; i += LANES) {
        const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc.data() + i));
        const __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(weights + i));
        const __m256i v = _mm256_min_epi16(_mm256_max_epi16(x, zero), clip);
        sum             = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_mullo_epi16(v, w), v));
    }
    return horizontalSum(sum);
}
#else
// Square Clipped ReLU: clamp to [0, QA], then square (so the result is in QA^2 units).
int32_t screlu(int16_t x) {
    const int32_t v = std::clamp<int32_t>(x, 0, QA);
    return v * v;
}

// Portable form of the AVX2 activatedDot above, with the same result. Kept as a
// single-stream reduction because MinGW GCC 13 leaves the two-perspective form of this
// loop scalar, ~3x slower; this form auto-vectorises everywhere.
int32_t activatedDot(const std::array<int16_t, HIDDEN>& acc, const int16_t* weights) {
    int32_t sum = 0;
    for (int i = 0; i < HIDDEN; ++i)
        sum += screlu(acc[i]) * weights[i];
    return sum;
}
#endif

bool outputWeightsInRange(const Network& net) {
    const auto rowInRange = [](const auto& row) {
        return std::ranges::all_of(row, [](int16_t w) { return std::abs(w) <= MAX_OUTPUT_WEIGHT; });
    };
    return std::ranges::all_of(net.outputWeights, rowInRange);
}

// `net` when activatedDot evaluates it exactly, else nullptr.
std::unique_ptr<Network> checked(std::unique_ptr<Network> net) {
    return outputWeightsInRange(*net) ? std::move(net) : nullptr;
}
} // namespace

std::unique_ptr<Network> loadNetwork(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < NETWORK_BYTES) return nullptr;
    in.seekg(0);
    auto net = std::make_unique<Network>();
    in.read(reinterpret_cast<char*>(net.get()), NETWORK_BYTES);
    return in ? checked(std::move(net)) : nullptr;
}

std::unique_ptr<Network> networkFromBytes(const unsigned char* data, std::size_t size) {
    if (data == nullptr || size < static_cast<std::size_t>(NETWORK_BYTES)) return nullptr;
    auto net = std::make_unique<Network>();
    std::memcpy(net.get(), data, static_cast<std::size_t>(NETWORK_BYTES));
    return checked(std::move(net));
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
    for (const Color perspective : {Color::WHITE, Color::BLACK})
        rebuild(net, board, Delta{}, perspective, viewOf(perspective, board.kingSq(perspective)),
                out.values[static_cast<int>(perspective)]);
}

void update(const Network& net, const Board& board, Move move, const Accumulator& parent,
            Accumulator& child) {
    const Delta d     = moveDelta(board, move);
    const Color mover = board.sideToMove();
    for (const Color perspective : {Color::WHITE, Color::BLACK}) {
        const int  p      = static_cast<int>(perspective);
        const View before = viewOf(perspective, board.kingSq(perspective));
        const View after = perspective == mover ? viewOf(perspective, kingAfter(board, d)) : before;

        // The mover's king changed bucket or side: every column moves, so rebuild.
        if (after != before) {
            rebuild(net, board, d, perspective, after, child.values[p]);
            continue;
        }

        std::array<int, 2> add{};
        std::array<int, 2> sub{};
        for (int i = 0; i < d.nAdded; ++i)
            add[i] = featureIndex(perspective, before, d.added[i]);
        for (int i = 0; i < d.nRemoved; ++i)
            sub[i] = featureIndex(perspective, before, d.removed[i]);

        if (d.nAdded == 2)
            applyDelta<2, 2>(net, parent.values[p], child.values[p], add, sub);
        else if (d.nRemoved == 2)
            applyDelta<1, 2>(net, parent.values[p], child.values[p], add, sub);
        else
            applyDelta<1, 1>(net, parent.values[p], child.values[p], add, sub);
    }
}

Value evaluate(const Network& net, const Accumulator& acc, const Board& board) {
    const Color stm     = board.sideToMove();
    const auto& ours    = acc.values[static_cast<int>(stm)];
    const auto& theirs  = acc.values[static_cast<int>(~stm)];
    const int   bucket  = outputBucket(board);
    const auto& weights = net.outputWeights[bucket];

    // int32 is the reference implementation's accumulator width; real activations stay
    // far below its range (the theoretical worst case does not occur in play).
    const int32_t sum =
        activatedDot(ours, weights.data()) + activatedDot(theirs, weights.data() + HIDDEN);

    // QA^2 * QB units -> QA * QB, add the bias, then scale to centipawns.
    const int32_t output = (sum / QA + net.outputBias[bucket]) * SCALE / (QA * QB);
    return std::clamp<Value>(output, VALUE_MATED_IN_MAX_PLY + 1, VALUE_MATE_IN_MAX_PLY - 1);
}

} // namespace nnue
} // namespace engine
