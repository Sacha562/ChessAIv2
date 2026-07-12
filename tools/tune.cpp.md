# Tool: `tune` (Texel eval tuner)

Offline tuner that fits the evaluation's piece-square tables
([eval](../src/eval.hpp.md)) to game results using **Texel's tuning method** — the
highest Elo-per-hour step in the Phase 1c HCE work ([PLAN.md](../PLAN.md) §18). It is
a standalone command-line program (`tuner`), **not** part of the engine: it links
only `src/eval.cpp` (for `EvalParams` and the seed tables) plus the header-only
library. Heap, I/O, and floating point are all fine here — none of this runs on the
search path.

## Source

- **Source:** [tune.cpp](tune.cpp)
- **Build target:** `tuner` (see [build.md](../build.md#tuning-tools)) — `make tuner`
  or the CMake `tuner` target.
- **Reads:** [`engine::EvalParams` / `DEFAULT_EVAL_PARAMS`](../src/eval.hpp.md#types)
  as the starting weights.
- **Companion tool:** [extract.cpp.md](extract.cpp.md) produces the dataset it
  consumes.

## What it does

Minimizes the logistic loss over labeled quiet positions:

```
E = mean( (result - sigmoid(K * q))^2 )    sigmoid(x) = 1 / (1 + 10^(-K*x/400))
```

where `q` is the **White-relative** static eval of a position and
`result ∈ {1.0, 0.5, 0.0}` is the game outcome from White's perspective.

Because the eval is a **phase-tapered linear sum of table entries**, the gradient of
`E` with respect to each weight is exact and cheap, so the tuner uses **gradient
descent (Adam)** rather than classic ±1 local search — it converges in seconds on
in-memory data. Steps:

1. **Load** the dataset, parsing each `FEN result` line once into a compact feature
   list (`(pieceType, table index, sign)` per piece) plus the position's phase
   fractions. In-check positions are dropped (the static eval is unreliable there).
2. **Fit K** once by a ternary search that minimizes `E` on the seed weights, then
   freeze it (or accept a fixed `--k`).
3. **Descend** for `--epochs` Adam steps: each epoch walks the dataset, accumulates
   the exact gradient, and updates all 768 table entries. The loss is printed
   periodically.
4. **Emit** the tuned tables as paste-ready C++ `constexpr std::array` definitions
   (a8-first, material folded in) on stdout.

## Usage

```
tuner <dataset> [--epochs N] [--lr F] [--k F] [--sample N]
```

| Argument | Default | Meaning |
|----------|---------|---------|
| `<dataset>` | — | Text file, one `FEN result` per line (`extract` output). |
| `--epochs N` | 300 | Number of Adam epochs. |
| `--lr F` | 2.0 | Adam learning rate, in centipawns. |
| `--k F` | fit | Fix the sigmoid scale `K` instead of fitting it. |
| `--sample N` | all | Use only the first `N` positions (0 = all). |

Progress (position count, fitted `K`, start/periodic loss) goes to stdout ahead of
the table dump.

## Tuning workflow

```bash
# 1. self-play games (fastchess) -> PGN
# 2. PGN -> labeled quiet positions
./extract games/selfplay.pgn > dataset.txt
# 3. fit the tables
./tuner dataset.txt --epochs 300 > tuned.txt
# 4. paste the emitted MG_*/EG_* arrays into src/eval.cpp (replacing the seeds),
#    rebuild, and SPRT-confirm the tuned eval vs the current one.
```

The emitted arrays are **combined** tables (material folded in), matching how the
tuner reads `DEFAULT_EVAL_PARAMS`. When adopting a tune, replace the
material-plus-delta seed block in [eval.cpp](../src/eval.hpp.md) with the combined
output (dropping the separate `MG_VALUE`/`EG_VALUE` fold), then confirm with SPRT
before committing — no eval change ships on the tuner's word alone.

## Notes & limitations

- **Data quality is everything.** The result label must reflect *this eval's*
  judgment, so tune on the engine's own self-play, not a GM/human database (the
  positions and outcomes are off-distribution). See [PLAN.md](../PLAN.md) §18.
- The quiet filter is currently "not in check" (plus `extract`'s pre-filter). A
  stronger filter — keep only positions whose static eval equals their quiescence
  eval — is a possible refinement.
- Single-threaded. Fast enough for datasets up to millions of positions; a threaded
  gradient pass is a straightforward later optimization if needed.
- Tunes **eval weights only.** Search constants (LMR/NMP/futility margins) are tuned
  separately by SPSA (see [PLAN.md](../PLAN.md) §18).
