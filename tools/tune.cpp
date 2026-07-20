// Texel tuner for the hand-crafted evaluation (src/eval.*).
//
// Fits every linear weight in engine::EvalParams -- the piece-square tables, the
// mobility tables, and the pawn-structure terms -- to game results by minimizing the
// logistic loss E = mean( (result - sigmoid(K * qi))^2 ), where qi is the
// White-relative static eval of a quiet position and result in {1, 0.5, 0} is the
// game outcome from White's perspective (Texel's tuning method).
//
// The eval is a *linear* function of its weights (a phase-tapered sum of table
// entries and pawn terms). The tuner exploits that: for each position it extracts a
// sparse coefficient vector c such that q = dot(theta, c) exactly, where theta is the
// flattened parameter vector. The gradient is then exact and cheap, and Adam
// converges in seconds. Because the extraction re-derives the eval independently, it
// is cross-checked against the engine's own evaluate() at load time (see
// faithfulness() / loadDataset) -- if the two ever disagree the tuner refuses to run,
// so it can never silently tune a different function than the engine plays.
//
// This is an offline tool -- heap and I/O are fine here (none of it runs on the
// search path).
//
// Usage:
//   tuner <dataset> [--epochs N] [--lr F] [--k F] [--sample N]
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "chess.hpp"
#include "eval.hpp"

using namespace chess;
using engine::DEFAULT_EVAL_PARAMS;
using engine::EvalParams;
using engine::SAFETY_DIM;

namespace {

// --- Flat parameter layout: EvalParams unrolled into one vector of NPARAMS doubles.
constexpr int N_PSQT = 6 * 64; // per phase
constexpr int N_MOB  = 4 * 28; // per phase (knight, bishop, rook, queen)
constexpr int N_PASS = 8;      // per phase (by relative rank)

constexpr int OFF_PSQT_MG  = 0;
constexpr int OFF_PSQT_EG  = OFF_PSQT_MG + N_PSQT; // 384
constexpr int OFF_MOB_MG   = OFF_PSQT_EG + N_PSQT; // 768
constexpr int OFF_MOB_EG   = OFF_MOB_MG + N_MOB;   // 880
constexpr int OFF_PASS_MG  = OFF_MOB_EG + N_MOB;   // 992
constexpr int OFF_PASS_EG  = OFF_PASS_MG + N_PASS; // 1000
constexpr int IDX_ISO_MG   = OFF_PASS_EG + N_PASS; // 1008
constexpr int IDX_ISO_EG   = IDX_ISO_MG + 1;       // 1009
constexpr int IDX_DBL_MG   = IDX_ISO_EG + 1;       // 1010
constexpr int IDX_DBL_EG   = IDX_DBL_MG + 1;       // 1011
constexpr int IDX_BP_MG    = IDX_DBL_EG + 1;       // 1012 bishop pair
constexpr int IDX_BP_EG    = IDX_BP_MG + 1;
constexpr int IDX_RO_MG    = IDX_BP_EG + 1; // rook open file
constexpr int IDX_RO_EG    = IDX_RO_MG + 1;
constexpr int IDX_RS_MG    = IDX_RO_EG + 1; // rook semi-open file
constexpr int IDX_RS_EG    = IDX_RS_MG + 1;
constexpr int IDX_R7_MG    = IDX_RS_EG + 1; // rook on 7th
constexpr int IDX_R7_EG    = IDX_R7_MG + 1;
constexpr int IDX_KO_MG    = IDX_R7_EG + 1; // knight outpost
constexpr int IDX_KO_EG    = IDX_KO_MG + 1;
constexpr int OFF_KS       = IDX_KO_EG + 1;       // 1022 king-safety danger table (mg-only)
constexpr int IDX_KSH_NEAR = OFF_KS + SAFETY_DIM; // 1047 king shield, near band (mg-only)
constexpr int IDX_KSH_FAR  = IDX_KSH_NEAR + 1;    // 1048 king shield, far band (mg-only)
constexpr int IDX_KST_NEAR = IDX_KSH_FAR + 1;     // 1049 king storm, near band (mg-only)
constexpr int IDX_KST_FAR  = IDX_KST_NEAR + 1;    // 1050 king storm, far band (mg-only)
constexpr int NPARAMS      = IDX_KST_FAR + 1;     // 1051

using Vec = std::array<double, NPARAMS>;

Vec flatten(const EvalParams& p) {
    Vec v{};
    for (int pt = 0; pt < 6; ++pt) {
        for (int sq = 0; sq < 64; ++sq) {
            v[OFF_PSQT_MG + pt * 64 + sq] = p.mg[pt][sq];
            v[OFF_PSQT_EG + pt * 64 + sq] = p.eg[pt][sq];
        }
    }
    for (int pc = 0; pc < 4; ++pc) {
        for (int m = 0; m < 28; ++m) {
            v[OFF_MOB_MG + pc * 28 + m] = p.mobMg[pc][m];
            v[OFF_MOB_EG + pc * 28 + m] = p.mobEg[pc][m];
        }
    }
    for (int r = 0; r < 8; ++r) {
        v[OFF_PASS_MG + r] = p.passedMg[r];
        v[OFF_PASS_EG + r] = p.passedEg[r];
    }
    v[IDX_ISO_MG] = p.isolatedMg;
    v[IDX_ISO_EG] = p.isolatedEg;
    v[IDX_DBL_MG] = p.doubledMg;
    v[IDX_DBL_EG] = p.doubledEg;
    v[IDX_BP_MG]  = p.bishopPairMg;
    v[IDX_BP_EG]  = p.bishopPairEg;
    v[IDX_RO_MG]  = p.rookOpenMg;
    v[IDX_RO_EG]  = p.rookOpenEg;
    v[IDX_RS_MG]  = p.rookSemiMg;
    v[IDX_RS_EG]  = p.rookSemiEg;
    v[IDX_R7_MG]  = p.rookSeventhMg;
    v[IDX_R7_EG]  = p.rookSeventhEg;
    v[IDX_KO_MG]  = p.knightOutpostMg;
    v[IDX_KO_EG]  = p.knightOutpostEg;
    for (int i = 0; i < SAFETY_DIM; ++i)
        v[OFF_KS + i] = p.kingDanger[i];
    v[IDX_KSH_NEAR] = p.kingShieldNearMg;
    v[IDX_KSH_FAR]  = p.kingShieldFarMg;
    v[IDX_KST_NEAR] = p.kingStormNearMg;
    v[IDX_KST_FAR]  = p.kingStormFarMg;
    return v;
}

// --- Eval-mirroring constants (must match src/eval.cpp exactly; verified at load) ---
const std::array<PieceType, 6> PIECE_TYPES = {PieceType::PAWN, PieceType::KNIGHT, PieceType::BISHOP,
                                              PieceType::ROOK, PieceType::QUEEN,  PieceType::KING};
const std::array<int, 6>       PHASE_WEIGHT = {0, 1, 1, 2, 4, 0};
constexpr int                  PHASE_MAX    = 24;
constexpr std::array<int, 4>   KS_WEIGHT    = {2, 2, 3, 5}; // king-safety attacker weights

constexpr uint64_t FILE_A_BITS = 0x0101010101010101ULL;

constexpr std::array<uint64_t, 8> buildFileBB() {
    std::array<uint64_t, 8> f{};
    for (int i = 0; i < 8; ++i)
        f[i] = FILE_A_BITS << i;
    return f;
}
constexpr std::array<uint64_t, 8> FILE_BB = buildFileBB();

constexpr std::array<uint64_t, 8> buildAdjFiles() {
    std::array<uint64_t, 8> a{};
    for (int i = 0; i < 8; ++i)
        a[i] = (i > 0 ? FILE_BB[i - 1] : 0) | (i < 7 ? FILE_BB[i + 1] : 0);
    return a;
}
constexpr std::array<uint64_t, 8> ADJ_FILES = buildAdjFiles();

constexpr std::array<uint64_t, 64> buildPassedMask(bool white) {
    std::array<uint64_t, 64> t{};
    for (int sq = 0; sq < 64; ++sq) {
        const int f = sq & 7, r = sq >> 3;
        uint64_t  m = 0;
        for (int ff = f - 1; ff <= f + 1; ++ff) {
            if (ff < 0 || ff > 7) continue;
            for (int rr = 0; rr < 8; ++rr)
                if (white ? rr > r : rr < r) m |= 1ULL << (rr * 8 + ff);
        }
        t[sq] = m;
    }
    return t;
}
constexpr std::array<uint64_t, 64> PASSED_MASK_W = buildPassedMask(true);
constexpr std::array<uint64_t, 64> PASSED_MASK_B = buildPassedMask(false);

// King pawn-shield masks (mirror of src/eval.cpp): the three files centred on the king
// (clamped inboard), on the rank `dist` steps in front of it.
constexpr std::array<uint64_t, 64> buildShieldMask(bool white, int dist) {
    std::array<uint64_t, 64> t{};
    for (int ks = 0; ks < 64; ++ks) {
        const int kf = ks & 7, kr = ks >> 3;
        const int center = std::clamp(kf, 1, 6);
        const int rr     = white ? kr + dist : kr - dist;
        if (rr < 0 || rr > 7) continue;
        uint64_t m = 0;
        for (int ff = center - 1; ff <= center + 1; ++ff)
            m |= 1ULL << (rr * 8 + ff);
        t[ks] = m;
    }
    return t;
}
constexpr std::array<uint64_t, 64> SHIELD_NEAR_W = buildShieldMask(true, 1);
constexpr std::array<uint64_t, 64> SHIELD_FAR_W  = buildShieldMask(true, 2);
constexpr std::array<uint64_t, 64> SHIELD_NEAR_B = buildShieldMask(false, 1);
constexpr std::array<uint64_t, 64> SHIELD_FAR_B  = buildShieldMask(false, 2);
constexpr std::array<uint64_t, 64> STORM_NEAR_W  = buildShieldMask(true, 2);
constexpr std::array<uint64_t, 64> STORM_FAR_W   = buildShieldMask(true, 3);
constexpr std::array<uint64_t, 64> STORM_NEAR_B  = buildShieldMask(false, 2);
constexpr std::array<uint64_t, 64> STORM_FAR_B   = buildShieldMask(false, 3);

// --- A training position as a sparse coefficient vector over theta. ---
struct Sample {
    std::vector<std::pair<int, double>> coef; // (param index, coefficient incl. sign * frac)
    double                              result = 0.0;
};

// Record a term contributing `count` (already signed) to a paired mg/eg parameter,
// phase-folded so it lands directly in q.
void addTerm(std::vector<std::pair<int, double>>& c, int mgIdx, int egIdx, double count,
             double mgFrac, double egFrac) {
    if (count == 0.0) return;
    c.emplace_back(mgIdx, count * mgFrac);
    c.emplace_back(egIdx, count * egFrac);
}

// Extract the full coefficient vector for `board`, mirroring engine::evaluate.
Sample makeSample(const Board& board, double result) {
    Sample s;
    s.result = result;

    // Phase first (needed for the mg/eg blend of every term).
    int phase = 0;
    for (int pt = 0; pt < 6; ++pt)
        phase += PHASE_WEIGHT[pt] * (board.pieces(PIECE_TYPES[pt], Color::WHITE).count() +
                                     board.pieces(PIECE_TYPES[pt], Color::BLACK).count());
    if (phase > PHASE_MAX) phase = PHASE_MAX;
    const double mgF = static_cast<double>(phase) / PHASE_MAX;
    const double egF = static_cast<double>(PHASE_MAX - phase) / PHASE_MAX;

    // PSQT.
    for (int pt = 0; pt < 6; ++pt) {
        Bitboard w = board.pieces(PIECE_TYPES[pt], Color::WHITE);
        while (w) {
            const int sq = w.pop() ^ 56;
            addTerm(s.coef, OFF_PSQT_MG + pt * 64 + sq, OFF_PSQT_EG + pt * 64 + sq, 1.0, mgF, egF);
        }
        Bitboard b = board.pieces(PIECE_TYPES[pt], Color::BLACK);
        while (b) {
            const int sq = static_cast<int>(b.pop());
            addTerm(s.coef, OFF_PSQT_MG + pt * 64 + sq, OFF_PSQT_EG + pt * 64 + sq, -1.0, mgF, egF);
        }
    }

    // Mobility.
    using CU           = Color::underlying;
    const Bitboard occ = board.occ();
    const Bitboard wp  = board.pieces(PieceType::PAWN, Color::WHITE);
    const Bitboard bp  = board.pieces(PieceType::PAWN, Color::BLACK);
    const Bitboard wPawnAt =
        attacks::pawnLeftAttacks<CU::WHITE>(wp) | attacks::pawnRightAttacks<CU::WHITE>(wp);
    const Bitboard bPawnAt =
        attacks::pawnLeftAttacks<CU::BLACK>(bp) | attacks::pawnRightAttacks<CU::BLACK>(bp);
    const std::array<Bitboard, 2> mobArea = {~board.us(Color::WHITE) & ~bPawnAt,
                                             ~board.us(Color::BLACK) & ~wPawnAt};
    const std::array<Color, 2>    colors  = {Color::WHITE, Color::BLACK};
    for (int ci = 0; ci < 2; ++ci) {
        const Color  c   = colors[ci];
        const double sgn = (c == Color::WHITE) ? 1.0 : -1.0;
        Bitboard     n   = board.pieces(PieceType::KNIGHT, c);
        while (n) {
            const int m = (attacks::knight(Square(n.pop())) & mobArea[ci]).count();
            addTerm(s.coef, OFF_MOB_MG + 0 * 28 + m, OFF_MOB_EG + 0 * 28 + m, sgn, mgF, egF);
        }
        Bitboard bi = board.pieces(PieceType::BISHOP, c);
        while (bi) {
            const int m = (attacks::bishop(Square(bi.pop()), occ) & mobArea[ci]).count();
            addTerm(s.coef, OFF_MOB_MG + 1 * 28 + m, OFF_MOB_EG + 1 * 28 + m, sgn, mgF, egF);
        }
        Bitboard r = board.pieces(PieceType::ROOK, c);
        while (r) {
            const int m = (attacks::rook(Square(r.pop()), occ) & mobArea[ci]).count();
            addTerm(s.coef, OFF_MOB_MG + 2 * 28 + m, OFF_MOB_EG + 2 * 28 + m, sgn, mgF, egF);
        }
        Bitboard q = board.pieces(PieceType::QUEEN, c);
        while (q) {
            const int m = (attacks::queen(Square(q.pop()), occ) & mobArea[ci]).count();
            addTerm(s.coef, OFF_MOB_MG + 3 * 28 + m, OFF_MOB_EG + 3 * 28 + m, sgn, mgF, egF);
        }
    }

    // Pawn structure.
    for (int ci = 0; ci < 2; ++ci) {
        const Color    c      = colors[ci];
        const bool     white  = c == Color::WHITE;
        const double   sgn    = white ? 1.0 : -1.0;
        const uint64_t own    = board.pieces(PieceType::PAWN, c).getBits();
        const uint64_t enemy  = board.pieces(PieceType::PAWN, ~c).getBits();
        const auto&    passed = white ? PASSED_MASK_W : PASSED_MASK_B;
        for (uint64_t bb = own; bb != 0; bb &= bb - 1) {
            const int sq = std::countr_zero(bb);
            const int f = sq & 7, r = sq >> 3;
            if ((own & ADJ_FILES[f]) == 0) addTerm(s.coef, IDX_ISO_MG, IDX_ISO_EG, sgn, mgF, egF);
            if ((enemy & passed[sq]) == 0) {
                const int relRank = white ? r : 7 - r;
                addTerm(s.coef, OFF_PASS_MG + relRank, OFF_PASS_EG + relRank, sgn, mgF, egF);
            }
        }
        for (int f = 0; f < 8; ++f) {
            const int extra = std::popcount(own & FILE_BB[f]) - 1;
            if (extra > 0) addTerm(s.coef, IDX_DBL_MG, IDX_DBL_EG, sgn * extra, mgF, egF);
        }
    }

    // Piece terms.
    const std::array<Bitboard, 2> ownPawnAtt = {wPawnAt, bPawnAt};
    for (int ci = 0; ci < 2; ++ci) {
        const Color    c      = colors[ci];
        const bool     white  = c == Color::WHITE;
        const double   sgn    = white ? 1.0 : -1.0;
        const uint64_t ownP   = board.pieces(PieceType::PAWN, c).getBits();
        const uint64_t enemyP = board.pieces(PieceType::PAWN, ~c).getBits();
        if (board.pieces(PieceType::BISHOP, c).count() >= 2)
            addTerm(s.coef, IDX_BP_MG, IDX_BP_EG, sgn, mgF, egF);
        Bitboard rk = board.pieces(PieceType::ROOK, c);
        while (rk) {
            const int sq = static_cast<int>(rk.pop());
            const int f = sq & 7, r = sq >> 3;
            if ((ownP & FILE_BB[f]) == 0) {
                if ((enemyP & FILE_BB[f]) == 0)
                    addTerm(s.coef, IDX_RO_MG, IDX_RO_EG, sgn, mgF, egF);
                else
                    addTerm(s.coef, IDX_RS_MG, IDX_RS_EG, sgn, mgF, egF);
            }
            if ((white ? r : 7 - r) == 6) addTerm(s.coef, IDX_R7_MG, IDX_R7_EG, sgn, mgF, egF);
        }
        Bitboard kn = board.pieces(PieceType::KNIGHT, c);
        while (kn) {
            const int sq      = static_cast<int>(kn.pop());
            const int relRank = white ? (sq >> 3) : 7 - (sq >> 3);
            if (relRank < 3 || relRank > 5) continue;
            if (!ownPawnAtt[ci].check(sq)) continue;
            const uint64_t ahead =
                (white ? PASSED_MASK_W[sq] : PASSED_MASK_B[sq]) & ~FILE_BB[sq & 7];
            if ((enemyP & ahead) == 0) addTerm(s.coef, IDX_KO_MG, IDX_KO_EG, sgn, mgF, egF);
        }
    }

    // King safety (mg-only): danger index against each king; the white-relative eval
    // adds kingDanger[blackIdx] and subtracts kingDanger[whiteIdx].
    const auto dangerIdx = [&](Color defender) {
        const Square   kingSq = board.kingSq(defender);
        const Bitboard zone   = attacks::king(kingSq) | Bitboard::fromSquare(kingSq);
        const Color    atk    = ~defender;
        int            weight = 0, count = 0;
        const auto     tally = [&](int w) {
            weight += w;
            ++count;
        };
        Bitboard n = board.pieces(PieceType::KNIGHT, atk);
        while (n) {
            if (attacks::knight(Square(n.pop())) & zone) tally(KS_WEIGHT[0]);
        }
        Bitboard b = board.pieces(PieceType::BISHOP, atk);
        while (b) {
            if (attacks::bishop(Square(b.pop()), occ) & zone) tally(KS_WEIGHT[1]);
        }
        Bitboard rr = board.pieces(PieceType::ROOK, atk);
        while (rr) {
            if (attacks::rook(Square(rr.pop()), occ) & zone) tally(KS_WEIGHT[2]);
        }
        Bitboard qq = board.pieces(PieceType::QUEEN, atk);
        while (qq) {
            if (attacks::queen(Square(qq.pop()), occ) & zone) tally(KS_WEIGHT[3]);
        }
        return count >= 2 ? std::min(weight, SAFETY_DIM - 1) : 0;
    };
    s.coef.emplace_back(OFF_KS + dangerIdx(Color::BLACK), mgF);
    s.coef.emplace_back(OFF_KS + dangerIdx(Color::WHITE), -mgF);

    // King pawn shield (mg-only): friendly pawns on the near/far shelter bands in front
    // of each king; White adds, Black subtracts, phase-folded by mgF.
    for (int ci = 0; ci < 2; ++ci) {
        const Color    c      = colors[ci];
        const bool     white  = c == Color::WHITE;
        const double   sgn    = white ? 1.0 : -1.0;
        const uint64_t pawns  = board.pieces(PieceType::PAWN, c).getBits();
        const int      ks     = board.kingSq(c).index();
        const uint64_t nearBB = white ? SHIELD_NEAR_W[ks] : SHIELD_NEAR_B[ks];
        const uint64_t farBB  = white ? SHIELD_FAR_W[ks] : SHIELD_FAR_B[ks];
        const double   nNear  = std::popcount(pawns & nearBB);
        const double   nFar   = std::popcount(pawns & farBB);
        if (nNear != 0.0) s.coef.emplace_back(IDX_KSH_NEAR, sgn * nNear * mgF);
        if (nFar != 0.0) s.coef.emplace_back(IDX_KSH_FAR, sgn * nFar * mgF);
    }

    // King pawn storm (mg-only): enemy pawns on the near/far storm bands in front of each
    // king; White adds, Black subtracts, phase-folded by mgF.
    for (int ci = 0; ci < 2; ++ci) {
        const Color    c      = colors[ci];
        const bool     white  = c == Color::WHITE;
        const double   sgn    = white ? 1.0 : -1.0;
        const uint64_t enemy  = board.pieces(PieceType::PAWN, ~c).getBits();
        const int      ks     = board.kingSq(c).index();
        const uint64_t nearBB = white ? STORM_NEAR_W[ks] : STORM_NEAR_B[ks];
        const uint64_t farBB  = white ? STORM_FAR_W[ks] : STORM_FAR_B[ks];
        const double   nNear  = std::popcount(enemy & nearBB);
        const double   nFar   = std::popcount(enemy & farBB);
        if (nNear != 0.0) s.coef.emplace_back(IDX_KST_NEAR, sgn * nNear * mgF);
        if (nFar != 0.0) s.coef.emplace_back(IDX_KST_FAR, sgn * nFar * mgF);
    }
    return s;
}

double dot(const Sample& s, const Vec& theta) {
    double q = 0.0;
    for (const auto& [idx, c] : s.coef)
        q += theta[idx] * c;
    return q;
}

// Confirm the sparse trace reproduces the engine's own eval (within integer rounding)
// for `board` under the seed weights. Returns the absolute discrepancy in centipawns.
double faithfulness(const Board& board, const Sample& s, const Vec& seed) {
    const engine::Value e           = engine::evaluate(board, DEFAULT_EVAL_PARAMS, 0);
    const double        engineWhite = (board.sideToMove() == Color::WHITE) ? e : -e;
    return std::abs(dot(s, seed) - engineWhite);
}

double sigmoid(double k, double q) {
    return 1.0 / (1.0 + std::pow(10.0, -k * q / 400.0));
}

double meanSquaredError(const std::vector<Sample>& data, const Vec& theta, double k) {
    double sum = 0.0;
    for (const Sample& s : data) {
        const double d = s.result - sigmoid(k, dot(s, theta));
        sum += d * d;
    }
    return sum / static_cast<double>(data.size());
}

double tuneK(const std::vector<Sample>& data, const Vec& theta) {
    double lo = 0.0, hi = 3.0;
    for (int i = 0; i < 40; ++i) {
        const double m1 = lo + (hi - lo) / 3.0;
        const double m2 = hi - (hi - lo) / 3.0;
        if (meanSquaredError(data, theta, m1) < meanSquaredError(data, theta, m2))
            hi = m2;
        else
            lo = m1;
    }
    return (lo + hi) / 2.0;
}

constexpr double LN10_OVER_400 = 2.302585092994046 / 400.0;

// Parameters seen in fewer than this many positions stay frozen at their seed. Adam
// takes ~lr-sized steps regardless of how weak a gradient is, so a rarely-seen
// parameter drifts to a wild value over many epochs on pure noise; freezing the tail
// keeps those at their (sane) seeds.
constexpr int MIN_SUPPORT = 2000;

// Decoupled weight decay toward the seed (AdamW-style). Adam's momentum averages to
// ~0 for a sign-flipping (noise/confounded) gradient, so the decay dominates there and
// pulls the weight back to seed; for a persistent (real) signal the Adam step wins and
// the weight moves freely, up to ~1/WD from seed. This keeps the tune a refinement of
// the (already strong) PeSTO seeds rather than a free-for-all that overfits.
constexpr double WEIGHT_DECAY = 0.01;

void adamEpoch(const std::vector<Sample>& data, Vec& theta, const Vec& seed, double k, double lr,
               Vec& m, Vec& v, int t, const std::array<int, NPARAMS>& support) {
    Vec          grad{};
    const double scale = k * LN10_OVER_400;
    const double invN  = 1.0 / static_cast<double>(data.size());
    for (const Sample& s : data) {
        const double sig    = sigmoid(k, dot(s, theta));
        const double factor = 2.0 * (sig - s.result) * sig * (1.0 - sig) * scale * invN;
        for (const auto& [idx, c] : s.coef)
            grad[idx] += factor * c;
    }
    constexpr double B1 = 0.9, B2 = 0.999, EPS = 1e-8;
    const double     bc1 = 1.0 - std::pow(B1, t);
    const double     bc2 = 1.0 - std::pow(B2, t);
    for (int i = 0; i < NPARAMS; ++i) {
        if (support[i] < MIN_SUPPORT) continue; // too little data: keep the seed
        m[i] = B1 * m[i] + (1 - B1) * grad[i];
        v[i] = B2 * v[i] + (1 - B2) * grad[i] * grad[i];
        theta[i] -= lr * (m[i] / bc1) / (std::sqrt(v[i] / bc2) + EPS);
        theta[i] -= lr * WEIGHT_DECAY * (theta[i] - seed[i]); // decoupled decay to seed
    }
}

bool parseResult(const std::string& tok, double& out) {
    if (tok == "1.0" || tok == "1" || tok == "1-0")
        out = 1.0;
    else if (tok == "0.0" || tok == "0" || tok == "0-1")
        out = 0.0;
    else if (tok == "0.5" || tok == "1/2-1/2" || tok == "1/2")
        out = 0.5;
    else
        return false;
    return true;
}

// Load the dataset, building samples and verifying each mirrors the engine's eval.
std::vector<Sample> loadDataset(const std::string& path, size_t cap, bool& faithful) {
    std::vector<Sample> data;
    const Vec           seed = flatten(DEFAULT_EVAL_PARAMS);
    faithful                 = true;
    std::ifstream in(path);
    if (!in) {
        std::cerr << "tuner: cannot open dataset '" << path << "'\n";
        return data;
    }
    std::string line;
    size_t      skipped = 0, mismatches = 0;
    double      worst = 0.0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto lastSpace = line.find_last_of(" \t");
        if (lastSpace == std::string::npos) continue;
        double result;
        if (!parseResult(line.substr(lastSpace + 1), result)) {
            ++skipped;
            continue;
        }
        Board board(line.substr(0, lastSpace));
        if (board.inCheck()) continue; // non-quiet
        Sample       s    = makeSample(board, result);
        const double diff = faithfulness(board, s, seed);
        if (diff > 1.5) {
            ++mismatches;
            worst = std::max(worst, diff);
        }
        data.push_back(std::move(s));
        if (cap != 0 && data.size() >= cap) break;
    }
    if (skipped) std::cerr << "tuner: skipped " << skipped << " lines with bad results\n";
    if (mismatches) {
        std::cerr << "tuner: FAITHFULNESS FAILED -- " << mismatches
                  << " positions where the trace != engine eval (worst " << worst
                  << " cp). The tuner models a different function than the engine; fix "
                     "makeSample before tuning.\n";
        faithful = false;
    }
    return data;
}

// --- Output: unflatten theta and print paste-ready tables. ---
void emitPsqt(const Vec& theta) {
    const std::array<const char*, 6> mgN = {"MG_PAWN", "MG_KNIGHT", "MG_BISHOP",
                                            "MG_ROOK", "MG_QUEEN",  "MG_KING"};
    const std::array<const char*, 6> egN = {"EG_PAWN", "EG_KNIGHT", "EG_BISHOP",
                                            "EG_ROOK", "EG_QUEEN",  "EG_KING"};
    std::cout << "\n// --- tuned PSQT (combined material, a8-first) ---\n";
    for (int half = 0; half < 2; ++half) {
        const int   off  = half == 0 ? OFF_PSQT_MG : OFF_PSQT_EG;
        const auto& name = half == 0 ? mgN : egN;
        for (int pt = 0; pt < 6; ++pt) {
            std::cout << "constexpr std::array<int16_t, 64> " << name[pt] << " = {";
            for (int sq = 0; sq < 64; ++sq) {
                if (sq % 8 == 0) std::cout << "\n    ";
                std::cout << static_cast<int>(std::lround(theta[off + pt * 64 + sq])) << ",";
            }
            std::cout << "\n};\n";
        }
    }
    std::cout << "constexpr std::array<std::array<int16_t, 64>, 6> MG_TABLE = {MG_PAWN, MG_KNIGHT, "
                 "MG_BISHOP, MG_ROOK, MG_QUEEN, MG_KING};\n";
    std::cout << "constexpr std::array<std::array<int16_t, 64>, 6> EG_TABLE = {EG_PAWN, EG_KNIGHT, "
                 "EG_BISHOP, EG_ROOK, EG_QUEEN, EG_KING};\n";
}

// Emit a 4x28 mobility table as a paste-ready constexpr initializer.
void emitMob(const char* name, const Vec& theta, int off) {
    std::cout << "constexpr std::array<std::array<int16_t, 28>, 4> " << name << " = {{";
    for (int pc = 0; pc < 4; ++pc) {
        std::cout << "\n    {";
        for (int m = 0; m < 28; ++m)
            std::cout << static_cast<int>(std::lround(theta[off + pc * 28 + m])) << ",";
        std::cout << "},";
    }
    std::cout << "\n}};\n";
}

void emitArray8(const char* name, const Vec& theta, int off) {
    std::cout << "constexpr std::array<int16_t, 8> " << name << " = {";
    for (int i = 0; i < 8; ++i)
        std::cout << static_cast<int>(std::lround(theta[off + i])) << ",";
    std::cout << "};\n";
}

void emitOther(const Vec& theta) {
    std::cout << "\n// --- tuned mobility (index: knight, bishop, rook, queen) ---\n";
    emitMob("MOB_MG", theta, OFF_MOB_MG);
    emitMob("MOB_EG", theta, OFF_MOB_EG);
    std::cout << "\n// --- tuned pawn structure ---\n";
    emitArray8("PASSED_MG", theta, OFF_PASS_MG);
    emitArray8("PASSED_EG", theta, OFF_PASS_EG);
    std::cout << "constexpr int16_t ISOLATED_MG = " << std::lround(theta[IDX_ISO_MG])
              << ", ISOLATED_EG = " << std::lround(theta[IDX_ISO_EG]) << ";\n";
    std::cout << "constexpr int16_t DOUBLED_MG = " << std::lround(theta[IDX_DBL_MG])
              << ", DOUBLED_EG = " << std::lround(theta[IDX_DBL_EG]) << ";\n";
    std::cout << "\n// --- tuned piece terms ---\n";
    auto pair = [&](const char* name, int mgIdx, int egIdx) {
        std::cout << "p." << name << "Mg = " << std::lround(theta[mgIdx]) << "; p." << name
                  << "Eg = " << std::lround(theta[egIdx]) << ";\n";
    };
    pair("bishopPair", IDX_BP_MG, IDX_BP_EG);
    pair("rookOpen", IDX_RO_MG, IDX_RO_EG);
    pair("rookSemi", IDX_RS_MG, IDX_RS_EG);
    pair("rookSeventh", IDX_R7_MG, IDX_R7_EG);
    pair("knightOutpost", IDX_KO_MG, IDX_KO_EG);
    std::cout << "\n// --- tuned king safety (kingDanger table) ---\np.kingDanger = {";
    for (int i = 0; i < SAFETY_DIM; ++i)
        std::cout << std::lround(theta[OFF_KS + i]) << ",";
    std::cout << "};\n";
    std::cout << "p.kingShieldNearMg = " << std::lround(theta[IDX_KSH_NEAR])
              << "; p.kingShieldFarMg = " << std::lround(theta[IDX_KSH_FAR]) << ";\n";
    std::cout << "p.kingStormNearMg = " << std::lround(theta[IDX_KST_NEAR])
              << "; p.kingStormFarMg = " << std::lround(theta[IDX_KST_FAR]) << ";\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tuner <dataset> [--epochs N] [--lr F] [--k F] [--sample N] "
                     "[--focus BEGIN END]\n";
        return 1;
    }
    const std::string dataset = argv[1];
    int               epochs  = 300;
    double            lr      = 2.0;
    double            fixedK  = -1.0;
    size_t            cap     = 0;
    int               focusBegin = -1, focusEnd = -1; // tune only [BEGIN, END); -1 = all
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--epochs" && i + 1 < argc)
            epochs = std::stoi(argv[++i]);
        else if (a == "--lr" && i + 1 < argc)
            lr = std::stod(argv[++i]);
        else if (a == "--k" && i + 1 < argc)
            fixedK = std::stod(argv[++i]);
        else if (a == "--sample" && i + 1 < argc)
            cap = static_cast<size_t>(std::stoll(argv[++i]));
        else if (a == "--focus" && i + 2 < argc) {
            focusBegin = std::stoi(argv[++i]);
            focusEnd   = std::stoi(argv[++i]);
        }
    }

    bool                faithful = true;
    std::vector<Sample> data     = loadDataset(dataset, cap, faithful);
    if (!faithful) return 2;
    if (data.empty()) {
        std::cerr << "tuner: no usable positions\n";
        return 1;
    }
    std::cout << "tuner: " << data.size() << " positions, " << NPARAMS
              << " params (trace == engine eval verified)\n";

    // Per-parameter data support: how many trace entries touch each parameter. Low
    // support -> frozen at seed during descent (see MIN_SUPPORT).
    std::array<int, NPARAMS> support{};
    for (const Sample& s : data)
        for (const auto& [idx, c] : s.coef)
            ++support[idx];

    // Freeze the mobility and passed-by-rank tables this round: their high buckets
    // (large mobility counts, advanced passers) are inherently sparse and noisy, so as
    // free per-bucket weights they overfit to wild values. Their seeds are already
    // sound (mobility's SPRT'd at +58; passed is a monotonic ramp). A monotonicity-
    // aware tune of these tables is a later refinement.
    for (int i = OFF_MOB_MG; i < IDX_ISO_MG; ++i)
        support[i] = 0;
    // Freeze the king PSQT too: king placement is strongly confounded with the game
    // result (a lost side's king is often on odd squares) without being causal, so a
    // free king table overfits catastrophically (a first attempt drove it to +/-400 cp
    // and cost ~125 Elo). Its PeSTO seed is sound. Tune the remaining piece PSQTs plus
    // isolated/doubled, with weight decay pulling everything toward the seeds.
    for (int i = OFF_PSQT_MG + 5 * 64; i < OFF_PSQT_MG + 6 * 64; ++i)
        support[i] = 0;
    for (int i = OFF_PSQT_EG + 5 * 64; i < OFF_PSQT_EG + 6 * 64; ++i)
        support[i] = 0;

    // Optional --focus BEGIN END: freeze every parameter outside [BEGIN, END) so only that
    // slice moves. Used to tune a newly added term in isolation (e.g. the king pawn
    // shield/storm weights) without disturbing the other seeds -- interacting terms like the
    // king-safety danger table must be held, or the isolated slice fits against a moving target.
    if (focusBegin >= 0) {
        for (int i = 0; i < NPARAMS; ++i)
            if (i < focusBegin || i >= focusEnd) support[i] = 0;
    }

    int frozen = 0;
    for (int i = 0; i < NPARAMS; ++i)
        if (support[i] < MIN_SUPPORT) ++frozen;

    const Vec    seed  = flatten(DEFAULT_EVAL_PARAMS);
    Vec          theta = seed;
    const double k     = (fixedK > 0.0) ? fixedK : tuneK(data, theta);
    std::cout << "tuner: K = " << k << ", start loss = " << meanSquaredError(data, theta, k) << ", "
              << frozen << "/" << NPARAMS << " params frozen (low support)\n";

    Vec m{}, v{};
    for (int t = 1; t <= epochs; ++t) {
        adamEpoch(data, theta, seed, k, lr, m, v, t, support);
        if (t % 25 == 0 || t == epochs)
            std::cout << "epoch " << t << " loss " << meanSquaredError(data, theta, k) << "\n";
    }

    emitPsqt(theta);
    emitOther(theta);
    return 0;
}
