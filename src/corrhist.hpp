#pragma once
#include <cstdint>
#include <memory>
#include "chess.hpp"
#include "types.hpp"

//
// Correction history (Phase 1d, widened after v0.6): a learned correction to the static
// evaluation, read from several tables at once.
//
// The static eval is a guess; the search is the ground truth. Whenever a node finishes,
// the gap between the two (`searchScore - correctedEval`) is a residual the eval still
// gets wrong in *this kind of position*. Each table remembers that residual for one view
// of the position, and the sum of all views is added back the next time a similar
// position is evaluated — so reverse futility, futility, null-move pruning and the
// `improving` trend all reason about an eval the search has already corrected.
//
// The views, each keyed by a cheap multiply-mix of a few bitboards (a couple of ALU ops
// per table on the per-node path; collisions only blur two buckets together):
//   - pawn     — the pawn skeleton of both sides (what an eval judges worst);
//   - non-pawn — one key per colour over that side's knights, bishops, rooks, queens and
//                king: two lookups, so each side's piece placement is learned on its own;
//   - minor    — knights, bishops and kings of both sides;
//   - major    — rooks, queens and kings of both sides;
//   - continuation — the previous move's (piece, to-square): the eval's blind spot for
//                the threat or tension that move just created.
// Every table is split by side to move, since scores are side-to-move relative.
//
// Entries learn with the gravity update every other history table uses, against the
// *corrected* eval, so the tables cooperate: their weighted sum converges on the residual
// instead of each one claiming all of it.
//
// A `CorrectionHistory` is owned per `Searcher` (per search), so it is single-thread
// state today and per-thread state under Lazy SMP later — no sharing, no locks.
//
namespace engine {

using chess::Color;

// Fixed-point scale of a stored entry: an entry holds `centipawns * CORR_GRAIN`, so the
// learned value keeps sub-centipawn resolution inside its int16_t slot.
constexpr int CORR_GRAIN = 256;

// Saturating bound on one stored entry: +/-64 cp. Small on purpose; each table nudges the
// eval, it does not overrule it.
constexpr int CORR_LIMIT = 64 * CORR_GRAIN;

// Table weights in percent: a table's entry counts `weight / 100` of its value toward the
// correction. 0 switches a table off (it still learns, but is never read).
struct CorrWeights {
    int pawn    = 100;
    int nonPawn = 100; // applied to each colour's lookup
    int minor   = 100;
    int major   = 100;
    int cont    = 100;
};

// The bucket a position reads and writes in every table, hashed once per node so the
// read at the static eval and the write after the move loop agree.
struct CorrSlots {
    int stm        = 0;  // side to move (0 = white)
    int pawn       = 0;  // pawn-table index
    int nonPawn[2] = {}; // non-pawn-table index per colour
    int minor      = 0;  // minor-table index
    int major      = 0;  // major-table index
    int contPiece  = -1; // previous move's piece (colour+type 0..11), -1 = none
    int contTo     = 0;  // previous move's to-square
};

// Cheap multiply-mix hashes of piece subsets that bucket the tables. Public so tests can
// assert each bucketing contract.
[[nodiscard]] uint64_t pawnStructureKey(const Board& board);
[[nodiscard]] uint64_t nonPawnKey(const Board& board, Color color);
[[nodiscard]] uint64_t minorKey(const Board& board);
[[nodiscard]] uint64_t majorKey(const Board& board);

// Per-side-to-move learned residuals of the static eval, over the five views above.
// Zeroed on construction, so a fresh search starts with an uncorrected eval.
class CorrectionHistory {
public:
    CorrectionHistory();

    // Reset every bucket to "no correction known".
    void clear();

    // Weights for the next search (Tunables::corr*Weight).
    void setWeights(const CorrWeights& weights) { weights_ = weights; }

    // Hash `board` into its bucket in every table. `prevPiece` / `prevTo` are the move that
    // led here (`prevPiece` -1 when there is none: the root, or after a null move).
    [[nodiscard]] static CorrSlots slots(const Board& board, int prevPiece, int prevTo);

    // The weighted sum (cp) of every table's entry for `s`. Hot path: five array reads.
    [[nodiscard]] Value correction(const CorrSlots& s) const;

    // `raw` (a side-to-move-relative static eval) plus that correction, clamped to stay
    // clear of the mate range so a corrected eval can never look like a forced mate.
    [[nodiscard]] Value corrected(const CorrSlots& s, Value raw) const;

    // Teach every table the residual `diff = searchScore - correctedEval` (cp) seen at a
    // node of `depth`: a deeper node's disagreement is more trustworthy, so it moves the
    // entries further. Called once per completed non-check node whose best move was quiet
    // and whose bound agrees with the residual's sign.
    void update(const CorrSlots& s, int depth, Value diff);

private:
    static constexpr int SIZE        = 16384; // buckets per side to move (power of two)
    static constexpr int MAX_WEIGHT  = 16;    // depth at which a residual carries full weight
    static constexpr int BONUS_DIV   = 128;   // residual * depth weight / this = one step
    static constexpr int MAX_BONUS   = CORR_LIMIT / 4; // largest single step (entry units)
    static constexpr int NUM_PIECES  = 12;
    static constexpr int NUM_SQUARES = 64;

    // The tables, heap-owned: at ~330 KB they would crowd the search thread's stack, which
    // also holds the Searcher that owns this history.
    struct Tables {
        int16_t pawn[2][SIZE];
        int16_t nonPawn[2][2][SIZE]; // [side to move][colour keyed][bucket]
        int16_t minor[2][SIZE];
        int16_t major[2][SIZE];
        int16_t cont[2][NUM_PIECES][NUM_SQUARES];
    };

    static int bucket(uint64_t key);

    std::unique_ptr<Tables> tables_;
    CorrWeights             weights_;
};

} // namespace engine
