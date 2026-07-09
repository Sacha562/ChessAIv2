# Module: `history`

Quiet-move ordering heuristics (**Phase 1b step 1**). Beta-cutoffs reveal which quiet
moves tend to refute a position; recording that lets later nodes try those moves
earlier. Good quiet ordering is the hard prerequisite for the Phase 1b pruning layers
(null-move, LMR, late-move pruning), which all assume the best move is searched first.

A single `History` bundles the three cutoff-driven signals, all consumed by
[movepick](movepick.hpp.md) and updated by [search](search.hpp.md):

```
butterfly history   saturating per-(color, from, to) score of cutoff success
killer moves        the two most recent quiet cutoff moves at each ply
countermoves        the quiet refutation indexed by the opponent's previous move
```

A `History` is owned **per [`Searcher`](search.hpp.md#class-searcher)** — one search
worker, one table set — so it is single-thread state today and per-thread state under
Lazy SMP later (Phase 1d): no sharing, no locks on the per-node path. Because a fresh
`Searcher` is constructed for every `go` / bench position, the tables reset each move
(cross-move persistence is a future option once workers become long-lived).

## Source Files

- **Header (interface):** [history.hpp](history.hpp)
- **Source (implementation):** [history.cpp](history.cpp)

## Namespace

- Public API (`MAX_HISTORY`, `class History`) in namespace `engine`.
- The bonus tuning constants (`HIST_BONUS_MULT`, `HIST_BONUS_CAP`), `historyBonus`, and
  `applyBonus` live in an anonymous namespace in `history.cpp` — internal linkage, **not**
  public API. They are first-cut values, SPSA-tunable later (see [PLAN.md](../PLAN.md) §18).

## Constants

### `MAX_HISTORY`

`constexpr int MAX_HISTORY = 16384`. The saturating bound on a butterfly-history entry:
the gravity update keeps every entry inside `[-MAX_HISTORY, MAX_HISTORY]`. Public so
[movepick](movepick.hpp.md) can `static_assert` that its score buckets are spaced wide
enough that the history-scored quiet band never collides with the killer/countermove
tiers above it or the losing-capture bucket below it.

## Objects / Interfaces

### `class History`

Cutoff-driven move-ordering memory. Default-constructed empty (all-zero history,
all-`NO_MOVE` killers/counters) via `clear()`, so a fresh search starts clean.

| Member | Type | Description |
|--------|------|-------------|
| `butterfly_` | `int16_t[2][64][64]` | Per (color, from, to) cutoff score, bounded to `±MAX_HISTORY`. |
| `killers_` | `Move[MAX_PLY][2]` | Two killer moves per ply; slot 0 is the most recent. |
| `counters_` | `Move[12 * 64]` | Countermove keyed by the previous move's piece (0–11) × target square. |

**Methods:** [`clear`](#historyclear), [`quietScore`](#historyquietscore),
[`killer`](#historykiller), [`counter`](#historycounter),
[`updateQuietCutoff`](#historyupdatequietcutoff) (public);
`counterIndex` (private helper).

**Used by:** [`Searcher`](search.hpp.md#class-searcher) (owns one),
[`orderMoves`](movepick.hpp.md#ordermoves) (reads the queries).

## Functions

### `History::clear`

Reset every table to empty. Called by the constructor; available for reuse/tests.
`butterfly_` is zeroed; every killer and countermove slot is set to `Move::NO_MOVE`.

### `History::quietScore`

Butterfly-history score of a quiet `move` for side `stm`, in
`[-MAX_HISTORY, MAX_HISTORY]`. Higher means "this move cut more often here before."

**Parameters:** `stm` (`Color`), `move` (`Move`, indexed by `from()`/`to()`).
**Returns:** `int`. Hot path — a plain array read.

### `History::killer`

The killer move stored in `slot` (`0` = most recent, `1` = older) at `ply`, or
`Move::NO_MOVE` if none. `ply` must be in `[0, MAX_PLY)`; the caller guarantees this by
clamping the root search depth to `MAX_PLY - 1` (see
[`Searcher::think`](search.hpp.md#searcherthink)) — with no extensions yet a node's ply
never exceeds the root depth.

### `History::counter`

The stored countermove refuting `prevMove` (the move that produced the current
position), or `Move::NO_MOVE` when there is no usable previous move. `board` is the
position **after** `prevMove`, so the moved piece sits on `prevMove.to()`; the slot is
keyed by that piece (color+type, 0–11) and the target square. Returns `NO_MOVE` when
`prevMove` is `NO_MOVE` or its target square is empty.

### `History::updateQuietCutoff`

Record a quiet-move fail-high. Called once per beta-cutoff **by a quiet move** from
[`Searcher::search`](search.hpp.md#searchersearch).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Current node position (after `prevMove`); used for countermove indexing. |
| `stm` | `Color` | Side to move at this node (the side making `cutoff`). |
| `ply` | `int` | Distance from root; indexes the killer slots. |
| `depth` | `int` | Remaining depth; drives the bonus magnitude. |
| `prevMove` | `Move` | The opponent's last move (`NO_MOVE` at the root child). |
| `cutoff` | `Move` | The quiet move that caused the cutoff. |
| `quietsTried` | `const Move*` | Quiet moves searched **before** `cutoff` that failed to cut. |
| `nTried` | `int` | Length of `quietsTried`. |

**Effects:** rewards `cutoff` in the butterfly table by `historyBonus(depth) =
min(HIST_BONUS_CAP, HIST_BONUS_MULT · depth²)`; penalises every move in `quietsTried`
by the same magnitude (the **malus**); pushes `cutoff` into the killer slots (newest in
slot 0, no duplicate); and stores `cutoff` as the countermove to `prevMove`.

**Warnings:** the butterfly update is a **gravity** update —
`entry += bonus − entry·|bonus|/MAX_HISTORY` — which drives the entry toward the bound
while damping as it approaches, so `|entry|` never exceeds `MAX_HISTORY` (with the bonus
itself clamped to that range). This bounds the entry inside its `int16_t` slot **and**
inside movepick's quiet score band. Only quiet cutoffs update history: capture cutoffs
are ordered by SEE/MVV-LVA and are not tracked here (capture history is a later step).
