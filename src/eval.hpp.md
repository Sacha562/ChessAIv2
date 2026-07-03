# Module: `eval`

Static position evaluation. Given a board, it returns a single centipawn
[`Value`](types.hpp.md#using-value--int) from the side-to-move's perspective, used
by [search](search.hpp.md) at leaf nodes (`depth <= 0`). This is a search **hot
path** — `evaluate` is called once per leaf, so it must stay cheap.

Phase 0 is material-only (plus a caller-supplied tempo bonus). PSQT, tapered eval,
pawn structure, mobility, and king safety arrive in Phase 1c (see
[PLAN.md](../PLAN.md) §17); NNUE in Phase 2.

## Source Files

- **Header (interface):** [eval.hpp](eval.hpp)
- **Source (implementation):** [eval.cpp](eval.cpp)

## Namespace

- Public API declared in namespace `engine`.
- Piece values (`V_PAWN` … `V_QUEEN`) and the `diff` helper live in an anonymous
  namespace in `eval.cpp` — internal linkage, **not** public API. The tempo bonus is
  no longer a constant here: it is passed in as the `tempo` argument (a self-play
  knob held by [`Tunables`](search.hpp.md#struct-tunables)).

## Functions

### `evaluate`

Compute the static evaluation of `board`.

**When to use:** at search leaf nodes, to score a quiet position. Phase 0 has no
quiescence, so it is called directly when `depth <= 0`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position to score. Read-only; not modified. |
| `tempo` | `Value` | Side-to-move bonus (centipawns), added after the perspective flip. A self-play-tunable knob; callers pass [`Tunables::tempo`](search.hpp.md#struct-tunables) (default `10`). |

**Returns:** [`Value`](types.hpp.md#using-value--int) — centipawns from the
side-to-move's perspective. Positive favors the side to move. Computed as the
white-minus-black material sum, negated for Black to move (negamax convention),
plus the `tempo` bonus.

**Side Effects:** none (pure function of `board`).

**Performance:** per-leaf hot path. Phase 0 is O(1) over piece-type popcounts via
`Board::pieces(pt, color).count()`.

**Warnings:** the tempo bonus is applied **after** the perspective flip, so it
always favors the side to move (correct). When adding asymmetric terms later,
respect the same side-relative sign convention.
