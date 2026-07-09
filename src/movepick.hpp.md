# Module: `movepick`

Move ordering (Phase 1a step 6, extended in **Phase 1b step 1**). Alpha-beta converges
far faster when the best move is searched first, and every later pruning layer assumes
it. This module scores and sorts a node's move list into the canonical order:

```
TT / hash move
> winning & equal captures (SEE >= 0), MVV-LVA within
> killer 1 > killer 2 > countermove
> remaining quiet moves, by butterfly-history score
> losing captures (SEE < 0), MVV-LVA within
```

It is a search **hot path** — called once per interior node — and allocates nothing
(the score scratch buffer is a fixed stack array). The killer / countermove / history
signals come from [`History`](history.hpp.md); continuation history arrives in a later
step. Used by [search](search.hpp.md) (both the main search and
[quiescence](search.hpp.md#searcherqsearch)).

## Source Files

- **Header (interface):** [movepick.hpp](movepick.hpp)
- **Source (implementation):** [movepick.cpp](movepick.cpp)

## Namespace

- Public API (`mvvLva`, `orderMoves`, `orderCaptures`) in namespace `engine`;
  [`History`](history.hpp.md) is forward-declared (passed by `const&`).
- The score buckets (`SCORE_TT`, `SCORE_GOOD_CAP`, `SCORE_KILLER1`, `SCORE_KILLER2`,
  `SCORE_COUNTER`, `SCORE_QUIET`, `SCORE_BAD_CAP`), the per-node `QuietOrder` scratch
  struct, `scoreMove`, and `sortByScore` live in an anonymous namespace in
  `movepick.cpp` — internal linkage, **not** public API. Two `static_assert`s pin the
  bucket spacing against [`MAX_HISTORY`](history.hpp.md#max_history) so the
  history-scored quiet band can never overlap the killer/countermove tiers above it or
  the losing-capture bucket below it.

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
`ttMove` (may be `Move::NO_MOVE`) is placed first when present in the list. The quiet
signals are read from `hist` **once per node** into a `QuietOrder` scratch struct
(killers at `ply`, the countermove to `prevMove`, the side to move), then each move is
scored once:

- TT move → `SCORE_TT`;
- captures → `SCORE_GOOD_CAP` / `SCORE_BAD_CAP` (chosen by
  [`seeGE`](see.hpp.md#seege)`(m, 0)`) plus their MVV-LVA;
- a quiet matching killer 1 / killer 2 / the countermove → `SCORE_KILLER1` /
  `SCORE_KILLER2` / `SCORE_COUNTER`;
- any other quiet → `SCORE_QUIET` plus its
  [`hist.quietScore`](history.hpp.md#historyquietscore) (in `±MAX_HISTORY`).

A stable insertion sort by descending score then reorders the list. Because captures
are classified before the killer/counter checks, a stored killer that happens to be a
capture in this position is scored as a capture (correct — killers are only ever stored
for quiet cutoffs).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position the moves belong to. |
| `moves` | `chess::Movelist&` | The generated legal moves; reordered in place (in/out). |
| `ttMove` | `Move` | Hash move to float to the front; `Move::NO_MOVE` if none. |
| `hist` | `const History&` | Quiet-move heuristics: killers, countermove, butterfly history. |
| `ply` | `int` | Distance from root; selects the killer slots. |
| `prevMove` | `Move` | Opponent's last move; keys the countermove (`NO_MOVE` if none). |

**Side Effects:** reorders `moves`. Calls `seeGE` once per capture; reads (never
writes) `hist`.

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
never interleave — an MVV-LVA score (max ≈ `16 * 900`) always fits inside a capture
bucket, and a butterfly-history score (`±MAX_HISTORY`) always fits inside the quiet
band strictly between the countermove tier and the losing-capture bucket (the two
`static_assert`s enforce this at compile time). The insertion sort is chosen because
move lists are short and it is **stable**, preserving generation order among
equal-scored moves (keeps `bench` deterministic).
