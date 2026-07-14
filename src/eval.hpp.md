# Module: `eval`

Static position evaluation. Given a board, it returns a single centipawn
[`Value`](types.hpp.md#using-value--int) from the side-to-move's perspective, used
by [search](search.hpp.md) at leaf nodes. This is a search **hot path** — `evaluate`
is called once per quiescence leaf, so it must stay cheap.

Phase 1c evaluation is a **tapered piece-square-table (PSQT) sum** with material
folded in (a "PeSTO"-style eval), plus a **mobility** term, a **pawn-structure** term
(isolated / doubled / passed), **piece terms** (bishop pair, rook on open/semi-open
file, rook on the 7th, knight outpost), and a caller-supplied tempo bonus. King safety
arrives later in Phase 1c (see [PLAN.md](../PLAN.md) §17); NNUE in Phase 2.
All weights are **Texel-tunable** — they live in a plain data block (`EvalParams`)
rather than hard-coded constants. Current defaults are **seeds**: PeSTO PSQTs
(already expertly tuned, kept as-is), a mobility ramp (SPRT-confirmed +58), and
pawn-structure seeds. A self-play Texel tune of the PSQT *regressed* (re-fitting
already-strong PeSTO on ~2000-Elo self-play moves it toward weaker play), so the
tuner ([tools/tune.cpp.md](../tools/tune.cpp.md)) is aimed at future *un-tuned* terms
(king safety, piece terms) rather than PeSTO.

## Source Files

- **Header (interface):** [eval.hpp](eval.hpp)
- **Source (implementation):** [eval.cpp](eval.cpp)

## Namespace

- Public API declared in namespace `engine`: the `EvalParams` struct, the
  `DEFAULT_EVAL_PARAMS` table, and the two `evaluate` overloads.
- The seed data — material (`MG_VALUE`/`EG_VALUE`), the twelve `MG_*`/`EG_*` PeSTO
  positional deltas and their `MG_DELTA`/`EG_DELTA` aggregates, the mobility ramp
  (`MOB_CENTER`/`MOB_MG_SLOPE`/`MOB_EG_SLOPE`/`MOB_MAX`) — plus the phase weights
  (`PHASE_WEIGHT`, `PHASE_MAX`), the `PIECE_TYPES` index map, the mobility indices
  (`MOB_KNIGHT`…`MOB_QUEEN`), the pawn-structure masks (`FILE_BB`, `ADJ_FILES`,
  `PASSED_MASK_W`/`PASSED_MASK_B` and their builders), the `buildDefaultParams` folder
  (folds material into the deltas and seeds the mobility/pawn/piece tables), and the
  `addMobility` / `addPawnStructure` / `addPieceTerms` helpers all live in an anonymous namespace in
  `eval.cpp` — internal linkage, **not** public API. The tempo bonus is not a constant
  here: it is passed in as the `tempo` argument (a self-play knob held by
  [`Tunables`](search.hpp.md#struct-tunables)).

## Types

### `struct EvalParams`

The tunable weight block: combined midgame/endgame piece-square tables with material
folded in.

| Member | Type | Description |
|--------|------|-------------|
| `mg` | `std::array<std::array<int16_t, 64>, 6>` | Midgame value of a `(piece type, square)`, in centipawns. |
| `eg` | `std::array<std::array<int16_t, 64>, 6>` | Endgame value of the same. |
| `mobMg` | `std::array<std::array<int16_t, 28>, 4>` | Midgame mobility bonus indexed by `(piece, safe-square count)`; piece 0=knight, 1=bishop, 2=rook, 3=queen. |
| `mobEg` | `std::array<std::array<int16_t, 28>, 4>` | Endgame mobility bonus, same indexing. |
| `passedMg` / `passedEg` | `std::array<int16_t, 8>` | Passed-pawn bonus by rank relative to the pawn's own side (0 = own back rank, 7 = promotion; both ends unused). |
| `isolatedMg` / `isolatedEg` | `int16_t` | Penalty (≤ 0) per pawn with no friendly pawn on an adjacent file. |
| `doubledMg` / `doubledEg` | `int16_t` | Penalty (≤ 0) per extra pawn sharing a file. |
| `bishopPairMg` / `bishopPairEg` | `int16_t` | Bonus for holding both bishops. |
| `rookOpenMg` / `rookOpenEg` | `int16_t` | Bonus for a rook on a file with no pawns of either colour. |
| `rookSemiMg` / `rookSemiEg` | `int16_t` | Bonus for a rook on a file with no friendly pawns (enemy pawns present). |
| `rookSeventhMg` / `rookSeventhEg` | `int16_t` | Bonus for a rook on its relative 7th rank. |
| `knightOutpostMg` / `knightOutpostEg` | `int16_t` | Bonus for a knight on the relative 4th–6th rank, pawn-defended, that no enemy pawn can attack. |

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

**Mobility:** each knight/bishop/rook/queen scores `mobMg`/`mobEg` indexed by how
many of its attacked squares fall in the side's **mobility area** — squares that are
neither occupied by a friendly piece nor attacked by an enemy pawn (sliders are
blocked by the full occupancy). The two colours' mobility is accumulated into the
same `mg`/`eg` sums (White adds, Black subtracts) before the taper, so it inherits
the phase blend. This is the second-biggest HCE term after PSQT and reuses the
library's `attacks::` tables.

**Pawn structure:** per pawn, an **isolated** penalty (no friendly pawn on an
adjacent file) and a **passed** bonus (no enemy pawn on the same or adjacent files
ahead, scaled by the pawn's relative rank); per file, a **doubled** penalty for each
extra pawn sharing it. Computed over the raw pawn bitboards against compile-time file
and passed-front-span masks. Accumulated into the same tapered `mg`/`eg` sums. A
**pawn hash** (caching this by pawn Zobrist, since pawn structure changes rarely) is a
planned NPS optimization — the term is recomputed from scratch for now.

**Piece terms:** the **bishop pair** bonus (≥ 2 bishops); per rook, an **open-file**
(no pawns of either colour) or **semi-open-file** (no friendly pawns) bonus and a
**7th-rank** bonus (relative rank 7); per knight, an **outpost** bonus when it sits on
the relative 4th–6th rank, is defended by a friendly pawn, and no enemy pawn can
advance to attack it (adjacent files ahead, reusing the passed-pawn masks). The
outpost test reuses the mobility pawn-attack bitboards, passed in per colour.

**Side Effects:** none (pure function of `board` and `params`).

**Performance:** per-leaf hot path. Recomputed from scratch each call — a bitboard
pop-loop per piece type for the PSQT sum; a sliding `attacks::` lookup per
knight/bishop/rook/queen plus a popcount for mobility; and a per-pawn scan plus
per-file popcounts for pawn structure. Mobility is the dominant added cost. Two
planned optimizations: an **incremental** PSQT sum on make/unmake (the discipline
NNUE's accumulator will reuse in Phase 2), and a **pawn hash** caching the
pawn-structure score. From-scratch is correct and simple for now.

**Warnings:**
- The tempo bonus is applied **after** the perspective flip, so it always favors the
  side to move (correct). When adding asymmetric terms later, respect the same
  side-relative sign convention.
- The colour-symmetry invariant `evaluate(pos, 0) == evaluate(mirror(pos), 0)` (for
  the vertical-mirror/colour-swap with side-to-move flipped) must hold for any table
  values — it is asserted in [test_eval.cpp](../tests/test_eval.cpp.md). A failure is
  an orientation bug in the `^ 56` mapping, not a tuning issue.
