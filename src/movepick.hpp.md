# Module: `movepick`

Move ordering (Phase 1a step 6). Alpha-beta converges far faster when the best move
is searched first, and every later pruning layer assumes it. This module scores and
sorts a node's move list into the canonical order:

```
TT / hash move
> winning & equal captures (SEE >= 0), MVV-LVA within
> quiet moves
> losing captures (SEE < 0), MVV-LVA within
```

It is a search **hot path** — called once per interior node — and allocates nothing
(the score scratch buffer is a fixed stack array). Killers, history, and continuation
history arrive in later steps. Used by [search](search.hpp.md) (both the main search
and [quiescence](search.hpp.md#searcherqsearch)).

## Source Files

- **Header (interface):** [movepick.hpp](movepick.hpp)
- **Source (implementation):** [movepick.cpp](movepick.cpp)

## Namespace

- Public API (`mvvLva`, `orderMoves`, `orderCaptures`) in namespace `engine`.
- The score buckets (`SCORE_TT`, `SCORE_GOOD_CAP`, `SCORE_QUIET`, `SCORE_BAD_CAP`),
  `scoreMove`, and `sortByScore` live in an anonymous namespace in `movepick.cpp` —
  internal linkage, **not** public API.

## Functions

### `mvvLva`

Most-Valuable-Victim / Least-Valuable-Attacker score of a capture:
`16 * pieceValue(victim) - pieceValue(attacker)`. The victim dominates; the attacker
breaks ties so the cheapest attacker is tried first. En passant is scored as a pawn
victim.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position the move is played in. |
| `move` | `Move` | The capture to score. |

**Returns:** `int` — higher is searched earlier.

### `orderMoves`

Order a **main-search** node's move list in place, best first (the order above).
`ttMove` (may be `Move::NO_MOVE`) is placed first when present in the list. Each move
is scored once — TT move → `SCORE_TT`; captures → `SCORE_GOOD_CAP` / `SCORE_BAD_CAP`
(chosen by [`seeGE`](see.hpp.md#seege)`(m, 0)`) plus their MVV-LVA; quiets →
`SCORE_QUIET` — then a stable insertion sort by descending score reorders the list.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position the moves belong to. |
| `moves` | `chess::Movelist&` | The generated legal moves; reordered in place (in/out). |
| `ttMove` | `Move` | Hash move to float to the front; `Move::NO_MOVE` if none. |

**Side Effects:** reorders `moves`. Calls `seeGE` once per capture.

### `orderCaptures`

Order a **quiescence** node's capture list in place by [`mvvLva`](#mvvlva) only. No
SEE split is applied — the q-search prunes losing captures itself (via `seeGE`) before
searching them, so a second SEE pass here would be wasted work.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position the captures belong to. |
| `moves` | `chess::Movelist&` | The generated captures; reordered in place (in/out). |

**Side Effects:** reorders `moves`.

**Warnings:** the score buckets are spaced by wide powers of two so the categories
never interleave — an MVV-LVA score (max ≈ `16 * 900`) always fits inside its bucket.
The insertion sort is chosen because move lists are short and it is **stable**,
preserving generation order among equal-scored moves (keeps `bench` deterministic).
