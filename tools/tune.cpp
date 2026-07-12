// Texel tuner for the hand-crafted evaluation (src/eval.*).
//
// Fits the piece-square tables in engine::EvalParams to game results by minimizing
// the logistic loss E = mean( (result - sigmoid(K * qi))^2 ), where qi is the
// White-relative static eval of a quiet position and result in {1, 0.5, 0} is the
// game outcome from White's perspective (Texel's tuning method).
//
// The eval is *linear* in the weights (a phase-tapered sum of table entries), so the
// gradient is exact and cheap: one pass over the dataset per epoch accumulates it
// directly, and Adam converges in seconds. This is an offline tool — heap, threads,
// and I/O are all fine here (none of this runs on the search path).
//
// Usage:
//   tuner <dataset> [--epochs N] [--lr F] [--k F] [--sample N]
//     <dataset>   text file, one "FEN result" per line; result = 1.0 | 0.5 | 0.0
//                 (White POV; '1-0' / '0-1' / '1/2-1/2' are also accepted).
//     --epochs N  Adam epochs (default 300).
//     --lr F      Adam learning rate in cp (default 2.0).
//     --k F       fix the sigmoid scale K instead of fitting it.
//     --sample N  use only the first N positions (0 = all; default all).
//
// Output: the fitted K, the final loss, and the tuned tables as paste-ready C++
// arrays (a8-first, material folded in) for src/eval.cpp.
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "chess.hpp"
#include "eval.hpp"

using namespace chess;
using engine::DEFAULT_EVAL_PARAMS;
using engine::EvalParams;

namespace {

constexpr int    NUM_PIECES    = 6;
constexpr int    NUM_SQ        = 64;
constexpr int    PHASE_MAX     = 24;
constexpr double LN10_OVER_400 = 2.302585092994046 / 400.0;

// One (piece type, table index, sign) contribution to a position's eval. sign = +1
// for a White piece, -1 for Black; idx is the a8-first table index the eval reads.
struct Feature {
    uint8_t pieceType;
    uint8_t index;
    int8_t  sign;
};

// A parsed training position: its features, game result, and tapered phase fractions.
struct Sample {
    std::vector<Feature> features;
    double               result; // 1.0 / 0.5 / 0.0, White POV
    double               mgFrac; // phase / 24
    double               egFrac; // (24 - phase) / 24
};

// Double-precision mirror of EvalParams for gradient work; [0] = mg, [1] = eg.
using Table = std::array<std::array<double, NUM_SQ>, NUM_PIECES>;
struct Weights {
    Table mg{};
    Table eg{};
};

const std::array<PieceType, NUM_PIECES> PIECE_TYPES  = {PieceType::PAWN,   PieceType::KNIGHT,
                                                        PieceType::BISHOP, PieceType::ROOK,
                                                        PieceType::QUEEN,  PieceType::KING};
const std::array<int, NUM_PIECES>       PHASE_WEIGHT = {0, 1, 1, 2, 4, 0};

Weights fromParams(const EvalParams& p) {
    Weights w;
    for (int pt = 0; pt < NUM_PIECES; ++pt) {
        for (int sq = 0; sq < NUM_SQ; ++sq) {
            w.mg[pt][sq] = p.mg[pt][sq];
            w.eg[pt][sq] = p.eg[pt][sq];
        }
    }
    return w;
}

// Parse a result token into a White-POV score, or return false if unrecognized.
bool parseResult(const std::string& tok, double& out) {
    if (tok == "1.0" || tok == "1" || tok == "1-0") {
        out = 1.0;
    } else if (tok == "0.0" || tok == "0" || tok == "0-1") {
        out = 0.0;
    } else if (tok == "0.5" || tok == "1/2-1/2" || tok == "1/2") {
        out = 0.5;
    } else {
        return false;
    }
    return true;
}

// Extract the features + phase for one board (mirrors engine::evaluate's mapping).
Sample makeSample(const Board& board, double result) {
    Sample s;
    s.result  = result;
    int phase = 0;
    for (int pt = 0; pt < NUM_PIECES; ++pt) {
        Bitboard white = board.pieces(PIECE_TYPES[pt], Color::WHITE);
        Bitboard black = board.pieces(PIECE_TYPES[pt], Color::BLACK);
        phase += PHASE_WEIGHT[pt] * (white.count() + black.count());
        while (white) {
            const int sq = white.pop() ^ 56; // White reads the a8-first table flipped
            s.features.push_back({static_cast<uint8_t>(pt), static_cast<uint8_t>(sq), 1});
        }
        while (black) {
            const int sq = static_cast<int>(black.pop()); // Black mirrors it
            s.features.push_back({static_cast<uint8_t>(pt), static_cast<uint8_t>(sq), -1});
        }
    }
    if (phase > PHASE_MAX) phase = PHASE_MAX;
    s.mgFrac = static_cast<double>(phase) / PHASE_MAX;
    s.egFrac = static_cast<double>(PHASE_MAX - phase) / PHASE_MAX;
    return s;
}

double whiteEval(const Sample& s, const Weights& w) {
    double q = 0.0;
    for (const Feature& f : s.features) {
        q += f.sign *
             (s.mgFrac * w.mg[f.pieceType][f.index] + s.egFrac * w.eg[f.pieceType][f.index]);
    }
    return q;
}

double sigmoid(double k, double q) {
    return 1.0 / (1.0 + std::pow(10.0, -k * q / 400.0));
}

double meanSquaredError(const std::vector<Sample>& data, const Weights& w, double k) {
    double sum = 0.0;
    for (const Sample& s : data) {
        const double d = s.result - sigmoid(k, whiteEval(s, w));
        sum += d * d;
    }
    return sum / static_cast<double>(data.size());
}

// Golden-section-style ternary search for the K that minimizes the loss.
double tuneK(const std::vector<Sample>& data, const Weights& w) {
    double lo = 0.0, hi = 3.0;
    for (int i = 0; i < 40; ++i) {
        const double m1 = lo + (hi - lo) / 3.0;
        const double m2 = hi - (hi - lo) / 3.0;
        if (meanSquaredError(data, w, m1) < meanSquaredError(data, w, m2))
            hi = m2;
        else
            lo = m1;
    }
    return (lo + hi) / 2.0;
}

// One Adam step of gradient descent on the logistic loss, K fixed.
void adamEpoch(const std::vector<Sample>& data, Weights& w, double k, double lr, Weights& m,
               Weights& v, int t) {
    Weights      grad;
    const double scale = k * LN10_OVER_400;
    const double invN  = 1.0 / static_cast<double>(data.size());

    for (const Sample& s : data) {
        const double sig    = sigmoid(k, whiteEval(s, w));
        const double factor = 2.0 * (sig - s.result) * sig * (1.0 - sig) * scale * invN;
        for (const Feature& f : s.features) {
            grad.mg[f.pieceType][f.index] += factor * f.sign * s.mgFrac;
            grad.eg[f.pieceType][f.index] += factor * f.sign * s.egFrac;
        }
    }

    constexpr double B1 = 0.9, B2 = 0.999, EPS = 1e-8;
    const double     bc1  = 1.0 - std::pow(B1, t);
    const double     bc2  = 1.0 - std::pow(B2, t);
    auto             step = [&](Table& weight, Table& mt, Table& vt, const Table& g) {
        for (int pt = 0; pt < NUM_PIECES; ++pt) {
            for (int sq = 0; sq < NUM_SQ; ++sq) {
                mt[pt][sq]      = B1 * mt[pt][sq] + (1 - B1) * g[pt][sq];
                vt[pt][sq]      = B2 * vt[pt][sq] + (1 - B2) * g[pt][sq] * g[pt][sq];
                const double mh = mt[pt][sq] / bc1;
                const double vh = vt[pt][sq] / bc2;
                weight[pt][sq] -= lr * mh / (std::sqrt(vh) + EPS);
            }
        }
    };
    step(w.mg, m.mg, v.mg, grad.mg);
    step(w.eg, m.eg, v.eg, grad.eg);
}

std::vector<Sample> loadDataset(const std::string& path, size_t cap) {
    std::vector<Sample> data;
    std::ifstream       in(path);
    if (!in) {
        std::cerr << "tuner: cannot open dataset '" << path << "'\n";
        return data;
    }
    std::string line;
    size_t      skipped = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // "FEN result": the FEN is the first 6 (or fewer) space-separated fields, the
        // result the last token.
        const auto lastSpace = line.find_last_of(" \t");
        if (lastSpace == std::string::npos) continue;
        const std::string fen = line.substr(0, lastSpace);
        const std::string tok = line.substr(lastSpace + 1);
        double            result;
        if (!parseResult(tok, result)) {
            ++skipped;
            continue;
        }
        Board board(fen);
        if (board.inCheck()) continue; // non-quiet: the eval is unreliable in check
        data.push_back(makeSample(board, result));
        if (cap != 0 && data.size() >= cap) break;
    }
    if (skipped) std::cerr << "tuner: skipped " << skipped << " lines with bad results\n";
    return data;
}

// Emit one table as a paste-ready C++ array (a8-first, combined material).
void emitTable(const char* name, const std::array<double, NUM_SQ>& t) {
    std::cout << "constexpr std::array<int16_t, 64> " << name << " = {";
    for (int sq = 0; sq < NUM_SQ; ++sq) {
        if (sq % 8 == 0) std::cout << "\n    ";
        std::cout << static_cast<int>(std::lround(t[sq])) << ",";
    }
    std::cout << "\n};\n";
}

void emitWeights(const Weights& w) {
    const std::array<const char*, NUM_PIECES> mgNames = {"MG_PAWN", "MG_KNIGHT", "MG_BISHOP",
                                                         "MG_ROOK", "MG_QUEEN",  "MG_KING"};
    const std::array<const char*, NUM_PIECES> egNames = {"EG_PAWN", "EG_KNIGHT", "EG_BISHOP",
                                                         "EG_ROOK", "EG_QUEEN",  "EG_KING"};
    std::cout << "\n// --- tuned tables (combined material, a8-first) ---\n";
    for (int pt = 0; pt < NUM_PIECES; ++pt)
        emitTable(mgNames[pt], w.mg[pt]);
    for (int pt = 0; pt < NUM_PIECES; ++pt)
        emitTable(egNames[pt], w.eg[pt]);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tuner <dataset> [--epochs N] [--lr F] [--k F] [--sample N]\n";
        return 1;
    }
    std::string dataset = argv[1];
    int         epochs  = 300;
    double      lr      = 2.0;
    double      fixedK  = -1.0;
    size_t      cap     = 0;
    for (int i = 2; i < argc - 1; ++i) {
        const std::string a = argv[i];
        if (a == "--epochs")
            epochs = std::stoi(argv[++i]);
        else if (a == "--lr")
            lr = std::stod(argv[++i]);
        else if (a == "--k")
            fixedK = std::stod(argv[++i]);
        else if (a == "--sample")
            cap = static_cast<size_t>(std::stoll(argv[++i]));
    }

    std::vector<Sample> data = loadDataset(dataset, cap);
    if (data.empty()) {
        std::cerr << "tuner: no usable positions\n";
        return 1;
    }
    std::cout << "tuner: " << data.size() << " positions\n";

    Weights w = fromParams(DEFAULT_EVAL_PARAMS);

    const double k = (fixedK > 0.0) ? fixedK : tuneK(data, w);
    std::cout << "tuner: K = " << k << ", start loss = " << meanSquaredError(data, w, k) << "\n";

    Weights m, v; // Adam moment estimates
    for (int t = 1; t <= epochs; ++t) {
        adamEpoch(data, w, k, lr, m, v, t);
        if (t % 25 == 0 || t == epochs)
            std::cout << "epoch " << t << " loss " << meanSquaredError(data, w, k) << "\n";
    }

    emitWeights(w);
    return 0;
}
