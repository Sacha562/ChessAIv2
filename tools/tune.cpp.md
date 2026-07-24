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

The eval is a **phase-tapered linear function of its weights**, so it fits *all*
[`EvalParams`](../src/eval.hpp.md#types) at once — the PSQT tables, the mobility
tables, the pawn-structure terms, the piece terms, the king-safety danger table, and
the king pawn-shield / pawn-storm weights (`NPARAMS` = 1051). All weights are flattened
into one vector `theta`, and each position is reduced to a **sparse coefficient
vector** `c` with `q = dot(theta, c)`. The gradient is then exact and cheap, so the
tuner uses **gradient descent (Adam)**, converging in seconds. Steps:

1. **Load** the dataset, parsing each `FEN result` line once. `makeSample` re-derives
   the eval's features (PSQT square, per-piece mobility count, isolated/doubled/passed
   pawn flags) into the coefficient vector. In-check positions are dropped.
2. **Verify faithfulness.** For every loaded position the reconstructed `dot(theta,c)`
   is checked against the engine's own `evaluate(board, DEFAULT_EVAL_PARAMS, 0)`. If
   any position disagrees by more than rounding (1.5 cp) the tuner **refuses to run**
   — it must model the exact function the engine plays, or a tune is worthless.
3. **Fit K** once by a ternary search that minimizes `E` on the seed weights, then
   freeze it (or accept a fixed `--k`).
4. **Descend** for `--epochs` Adam steps: each epoch walks the dataset, accumulates
   the exact gradient over the tunable parameters, and updates them. Loss printed
   periodically. **Overfit control** (see below) governs which parameters move and how
   far.
5. **Emit** the tuned weights: PSQT as paste-ready `constexpr std::array` tables
   (a8-first, material folded in), then the mobility tables and pawn-structure
   values.

## Overfit control (why a naive tune regresses)

Adam takes a ~`lr`-sized step whenever a gradient has a *consistent sign*, regardless
of how weak or confounded the signal is. On finite, shallow self-play data this drives
sparse or confounded weights to wild values that lower dataset MSE but **cost real
Elo** — a first attempt drove the king PSQT to ±400 cp and lost ~125 Elo. Two guards
keep the tune a *refinement* of the (already strong) PeSTO seeds:

- **Freeze low-signal tables.** Parameters seen in fewer than `MIN_SUPPORT` positions
  are held at their seed. Additionally the **king PSQT**, the **mobility** tables, and
  the **passed-pawn-by-rank** table are frozen outright: king placement is strongly
  *confounded* with the result without being causal, and the mobility/passed high
  buckets are inherently sparse — all overfit badly as free per-square/per-bucket
  weights. Their seeds are sound (mobility SPRT'd at +58; passed is a monotonic ramp).
- **Weight decay toward seed** (`WEIGHT_DECAY`, decoupled / AdamW-style). Each step also
  nudges every weight back toward its seed. Because Adam's momentum averages to ~0 for a
  sign-flipping (noise) gradient, the decay dominates there and holds the weight at
  seed; for a persistent (real) signal the Adam step wins and the weight moves freely,
  up to ~`1/WEIGHT_DECAY` from seed. This cleanly separates signal from overfit without
  a hard per-parameter cutoff.

Tuning the king PSQT, mobility, and passed tables properly needs a monotonicity-aware
fit and/or deeper data — a later refinement.

## Usage

```
tuner <dataset> [--epochs N] [--lr F] [--k F] [--sample N] [--focus BEGIN END]
```

| Argument | Default | Meaning |
|----------|---------|---------|
| `<dataset>` | — | Text file, one `FEN result` per line (`extract` output). |
| `--epochs N` | 300 | Number of Adam epochs. |
| `--lr F` | 2.0 | Adam learning rate, in centipawns. |
| `--k F` | fit | Fix the sigmoid scale `K` instead of fitting it. |
| `--sample N` | all | Use only the first `N` positions (0 = all). |
| `--focus BEGIN END` | off | Freeze every parameter outside the flat-index range `[BEGIN, END)`, so only that slice moves. Used to tune a newly added term (e.g. the king pawn shield/storm weights at indices `1047..1051`) in isolation, holding interacting seeds fixed. |

Progress (position count, fitted `K`, start/periodic loss) goes to stdout ahead of
the table dump.

## Tuning workflow

```bash
# 1. self-play games (fastchess) -> PGN
# 2. PGN -> labeled quiet positions
./extract games/selfplay.pgn > dataset.txt
# 3. fit the tables
./tuner dataset.txt --epochs 300 > tuned.txt
# 4. paste the emitted PSQT tables, mobility tables, and pawn-structure values into
#    src/eval.cpp (replacing the seeds), rebuild, and SPRT-confirm vs the current eval.
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
- **Models every current eval term** (PSQT + mobility + pawn structure + piece terms +
  king-safety table + king pawn-shield + king pawn-storm) via the faithfulness-checked trace,
  but by policy **actively tunes** only the non-king piece PSQTs, the isolated/doubled terms,
  the piece terms, the king-safety table, and the king pawn-shield / pawn-storm weights; the
  king PSQT, mobility, and passed-pawn tables are frozen (see
  [Overfit control](#overfit-control-why-a-naive-tune-regresses)). When a **new** eval
  term is added (e.g. king safety), `makeSample` and the parameter layout must be
  extended to include it — the load-time faithfulness check is the tripwire: it fails
  loudly if the trace and the engine eval drift apart (as it did when piece terms were
  first added to the eval but not the tuner), so an un-mirrored new term cannot be
  silently mis-tuned.
- The quiet filter is currently "not in check" (plus `extract`'s pre-filter). A
  stronger filter — keep only positions whose static eval equals their quiescence
  eval — is a possible refinement.
- Single-threaded. Fast enough for datasets up to millions of positions; a threaded
  gradient pass is a straightforward later optimization if needed.
- Tunes **eval weights only.** Search constants (LMR/NMP/futility margins) are tuned
  separately by SPSA (see [PLAN.md](../PLAN.md) §18).
