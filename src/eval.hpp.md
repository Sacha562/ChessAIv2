# Module: `eval`

Static position evaluation. Given a board, it returns a single centipawn
[`Value`](types.hpp.md#using-value--int) from the side-to-move's perspective, used
by [search](search.hpp.md) at leaf nodes. This is a search **hot path** — `evaluate`
is called once per quiescence leaf, so it must stay cheap.

Phase 1c evaluation is a **tapered piece-square-table (PSQT) sum** with material
folded in (a "PeSTO"-style eval), plus a caller-supplied tempo bonus. Pawn
structure, mobility, piece terms, and king safety arrive later in Phase 1c (see
[PLAN.md](../PLAN.md) §17); NNUE in Phase 2. All weights are **Texel-tunable** —
they live in a plain data block (`EvalParams`) rather than hard-coded constants, so
the tuner can fit them to self-play results and write the optimized tables back.

## Source Files

- **Header (interface):** [eval.hpp](eval.hpp)
- **Source (implementation):** [eval.cpp](eval.cpp)

## Namespace

- Public API declared in namespace `engine`: the `EvalParams` struct, the
  `DEFAULT_EVAL_PARAMS` table, and the two `evaluate` overloads.
- The PeSTO seed tables (`MG_VALUE`/`EG_VALUE` material, the twelve `MG_*`/`EG_*`
  positional deltas, `MG_DELTA`/`EG_DELTA`), the phase weights (`PHASE_WEIGHT`,
  `PHASE_MAX`), the `PIECE_TYPES` index map, and the `buildDefaultParams` folder all
  live in an anonymous namespace in `eval.cpp` — internal linkage, **not** public
  API. The tempo bonus is not a constant here: it is passed in as the `tempo`
  argument (a self-play knob held by [`Tunables`](search.hpp.md#struct-tunables)).

## Types

### `struct EvalParams`

The tunable weight block: combined midgame/endgame piece-square tables with material
folded in.

| Member | Type | Description |
|--------|------|-------------|
| `mg` | `std::array<std::array<int16_t, 64>, 6>` | Midgame value of a `(piece type, square)`, in centipawns. |
| `eg` | `std::array<std::array<int16_t, 64>, 6>` | Endgame value of the same. |

- **Piece index (0..5):** `P, N, B, R, Q, K`, matching `PIECE_TYPES` in `eval.cpp`.
- **Square orientation:** tables are stored in **White's a8-first view** (index 0 =
  a8, 63 = h1). `evaluate` reads a White piece at library square `s` as
  `mg[pt][s ^ 56]` (flip the rank) and a Black piece at `s` as `mg[pt][s]` (the
  vertical mirror), subtracting the latter. This is the classic PeSTO mapping and is
  what makes the eval colour-symmetric.
- **Combined tables:** each entry already includes the piece's material value, so
  there is no separate material term. `DEFAULT_EVAL_PARAMS` is `MG_VALUE[pt] +
  MG_DELTA[pt][sq]` (and the endgame analogue), summed at compile time by
  `buildDefaultParams`.

### `DEFAULT_EVAL_PARAMS`

`extern const EvalParams` — the PeSTO/Kaufman-seeded default tables, baked at
namespace scope. The search hot path evaluates against these; they are the starting
point Texel tuning refits. Copy it into a mutable `EvalParams` to perturb during
tuning.

## Functions

### `evaluate` (2-arg, search hot path)

`Value evaluate(const Board& board, Value tempo)` — evaluate against
`DEFAULT_EVAL_PARAMS`. This is the form the search calls.

### `evaluate` (3-arg, tuner)

`Value evaluate(const Board& board, const EvalParams& params, Value tempo)` — score
against a candidate parameter set. Used by the Texel tuner to measure the loss of a
perturbed table.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position to score. Read-only; not modified. |
| `params` | `const EvalParams&` | Weight tables to score with (3-arg form only). |
| `tempo` | `Value` | Side-to-move bonus (centipawns), added after the perspective flip. A self-play-tunable knob; callers pass [`Tunables::tempo`](search.hpp.md#struct-tunables) (default `9`). |

**Returns:** [`Value`](types.hpp.md#using-value--int) — centipawns from the
side-to-move's perspective. Positive favors the side to move. Computed as the
white-relative tapered PSQT sum, negated for Black to move (negamax convention),
plus the `tempo` bonus.

**Tapered eval:** a game-phase counter is accumulated from non-pawn material
(`PHASE_WEIGHT` = knight/bishop 1, rook 2, queen 4; both sides), clamped to
`PHASE_MAX = 24`. The returned score interpolates the midgame and endgame table
sums: `(mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX` (24 = pure opening, 0 =
bare endgame). This removes eval discontinuities and lets pawns and the king rise in
value toward the endgame.

**Side Effects:** none (pure function of `board` and `params`).

**Performance:** per-leaf hot path. Recomputed from scratch each call — one bitboard
pop-loop per piece type (~O(pieces on the board) table gathers) plus the phase
blend. An **incremental** PSQT sum on make/unmake (the discipline NNUE's accumulator
will reuse in Phase 2) is a later optimization; from-scratch is correct and simple
now.

**Warnings:**
- The tempo bonus is applied **after** the perspective flip, so it always favors the
  side to move (correct). When adding asymmetric terms later, respect the same
  side-relative sign convention.
- The colour-symmetry invariant `evaluate(pos, 0) == evaluate(mirror(pos), 0)` (for
  the vertical-mirror/colour-swap with side-to-move flipped) must hold for any table
  values — it is asserted in [test_eval.cpp](../tests/test_eval.cpp.md). A failure is
  an orientation bug in the `^ 56` mapping, not a tuning issue.
