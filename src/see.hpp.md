# Module: `see`

Static Exchange Evaluation (Phase 1a step 4). Given a move, SEE computes the net
material a side wins or loses by playing out the capture sequence on the move's
target square — both sides recapturing with their least-valuable attacker, either
free to stop once ahead. It is a **pure, read-only** query (the board is never
mutated) and a search **hot path**: it is consulted for every capture during move
ordering and quiescence pruning.

One primitive powers three consumers: the winning/losing capture split in
[move ordering](movepick.hpp.md), [quiescence](search.hpp.md#searcherqsearch) capture
pruning, and (later, Phase 1b) LMR / check-extension gating.

## Source Files

- **Header (interface):** [see.hpp](see.hpp)
- **Source (implementation):** [see.cpp](see.cpp)

## Namespace

- Public API (`SEE_PIECE_VALUE`, `pieceValue`, `seeGE`) in namespace `engine`.
- The `attackersTo` and `lsbBB` helpers live in an anonymous namespace in
  `see.cpp` — internal linkage, **not** public API.

## Objects / Interfaces

### `SEE_PIECE_VALUE`

`inline constexpr Value[]` — the material scale used across the search side (SEE,
[MVV-LVA](movepick.hpp.md#mvvlva), delta pruning). Indexed by `PieceType`
(`PAWN`=100, `KNIGHT`=320, `BISHOP`=330, `ROOK`=500, `QUEEN`=900, `KING`=0,
`NONE`=0). It is **intentionally independent of [eval](eval.hpp.md)'s values** —
SEE wants simple, stable integers, not whatever the evaluation is tuned to. The
king entry is never read (a king is never scored as captured or capturing; the
swap handles it structurally).

**Used by:** [`pieceValue`](#piecevalue), [movepick](movepick.hpp.md),
[search](search.hpp.md) (delta pruning).

## Functions

### `pieceValue`

Return the material value of a `PieceType` on the search-side scale
(`SEE_PIECE_VALUE`). `PieceType::NONE` maps to `0`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `pt` | `chess::PieceType` | Piece type to value. |

**Returns:** [`Value`](types.hpp.md#using-value--int) — centipawns.

### `seeGE`

Decide whether the static exchange evaluation of `move` on `board` is **at least
`threshold`** centipawns, from the moving side's perspective. This is the common
consumer query — `seeGE(board, move, 0)` answers "is this capture non-losing?" — and
is cheaper than a full signed SEE because it stops as soon as the sign relative to
`threshold` is settled (a balance-based swap after Stockfish's `see_ge`).

**When to use:** to classify a capture during move ordering (SEE ≥ 0 → winning/equal
bucket) or to prune a losing capture in quiescence (`!seeGE(m, 0)` → skip).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position the move is played in. Read-only; not modified. |
| `move` | `Move` | The move whose target-square exchange is evaluated. May be a capture, en passant, promotion, or a quiet move (a quiet move captures nothing, so it asks whether the moved piece is safe on its destination). |
| `threshold` | `Value` | The bar the exchange must clear. |

**Returns:** `bool` — `true` iff SEE(`move`) ≥ `threshold`.

**Algorithm:** the initiating capture's value seeds a running balance; then the loop
repeatedly takes with the side-to-move's least-valuable attacker (`attackersTo`
recomputes slider attacks through the shrinking occupancy, so **x-ray / battery**
pieces behind a moved slider are revealed), updating the balance and flipping the
side. A parity flag answers the threshold test on the first side that cannot improve.
Correctly handles:

- **En passant** — the captured pawn is removed from its actual square
  (`to.ep_square()`), not `to`.
- **Promotions** — a promotion capture adds the queening bonus (`Q − P`) and treats
  the mover as a queen thereafter. Under-promotions are scored as queens (a
  negligible SEE error).
- **The king** — a king may only capture into a square the opponent no longer
  defends; the final king branch enforces this.

**Side Effects:** none (pure function of `board`).

**Warnings:** **Pins are not modelled** — a pinned defender is still counted as an
attacker, so SEE can occasionally over-credit the defending side. This is a standard,
minor approximation (Stockfish itself long shipped without it). SEE is **minimax**:
because a side declines a losing recapture, SEE never exceeds the value of the first
captured piece — the early `threshold > captured` rejection relies on this.
