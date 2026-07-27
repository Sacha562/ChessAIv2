# Module: `history`

Quiet-move ordering heuristics (**Phase 1b step 1**; **continuation history** added in
**Phase 1c**). Beta-cutoffs reveal which quiet moves tend to refute a position; recording
that lets later nodes try those moves earlier. Good quiet ordering is the hard
prerequisite for the pruning layers (null-move, LMR, late-move pruning), which all assume
the best move is searched first.

A single `History` bundles the cutoff-driven signals, all consumed by
[movepick](movepick.hpp.md) and updated by [search](search.hpp.md):

```
butterfly history     saturating per-(color, from, to) score of cutoff success
killer moves          the two most recent quiet cutoff moves at each ply
countermoves          the quiet refutation indexed by the opponent's previous move
continuation history  saturating score for the current move conditioned on the piece+to
                      of a recent move (1 or 2 plies back) — a scored generalisation of
                      the countermove, and among the strongest modern quiet signals
capture history       saturating per-(moving piece, target square, victim) score of
                      cutoff success for captures (Phase 1d) — refines the SEE/MVV-LVA
                      order, which knows the material at stake but not whether the
                      capture actually works on this square
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

- Public API (`MAX_HISTORY`, `CONT_PLIES`, `struct ContHistContext`, `class History`) in
  namespace `engine`.
- The bonus tuning constants (`HIST_BONUS_MULT`, `HIST_BONUS_CAP`), `historyBonus`, and
  `applyBonus` live in an anonymous namespace in `history.cpp` — internal linkage, **not**
  public API. They are first-cut values, SPSA-tunable later (see [PLAN.md](../PLAN.md) §18).

## Constants

### `MAX_HISTORY`

`constexpr int MAX_HISTORY = 16384`. The saturating bound on a butterfly- **or**
continuation-history entry: the gravity update keeps every entry inside
`[-MAX_HISTORY, MAX_HISTORY]`. Public so [movepick](movepick.hpp.md) can `static_assert`
that its score buckets are spaced wide enough that the history-scored quiet band (butterfly
plus `CONT_PLIES` continuation entries) never collides with the killer/countermove tiers
above it or the losing-capture bucket below it.

### `CONT_PLIES`

`constexpr int CONT_PLIES = 2`. How many prior plies continuation history conditions on —
the moves 1 and 2 plies back. Public so [movepick](movepick.hpp.md) can size its quiet
score band as `(1 + CONT_PLIES) * MAX_HISTORY`.

## Objects / Interfaces

### `struct ContHistContext`

The resolved recent-move context a node hands to the continuation-history query and
update: the `(piece, to)` of the moves played 1 and 2 plies before it. Built by
[`Searcher::search`](search.hpp.md#searchersearch) from its per-ply search stack.

| Field | Type | Description |
|-------|------|-------------|
| `piece1` / `to1` | `int` | Move **one** ply ago: colour+type piece (0–11) and target square (0–63); `piece1 < 0` when absent. |
| `piece2` / `to2` | `int` | Move **two** plies ago; `piece2 < 0` when absent. |

A pair is absent (`piece < 0`) at the root, immediately after a null move, or before
enough plies exist. A default-constructed context (both `-1`) contributes nothing, which
is what [`qsearch`](search.hpp.md#searcherqsearch) passes.

### `class History`

Cutoff-driven move-ordering memory. Default-constructed empty (all-zero history,
all-`NO_MOVE` killers/counters) via `clear()`, so a fresh search starts clean.

| Member | Type | Description |
|--------|------|-------------|
| `butterfly_` | `int16_t[2][64][64]` | Per (color, from, to) cutoff score, bounded to `±MAX_HISTORY`. |
| `capture_` | `int16_t[12][64][6]` | Per (moving piece, target square, victim type) capture-cutoff score, bounded to `±MAX_HISTORY` (~9 KB). |
| `killers_` | `Move[MAX_PLY][2]` | Two killer moves per ply; slot 0 is the most recent. |
| `counters_` | `Move[12 * 64]` | Countermove keyed by the previous move's piece (0–11) × target square. |
| `continuation_` | `std::unique_ptr<ContTable>` | Continuation-history table `[priorPiece][priorTo][movedPiece][movedTo]` of `int16_t`, each bounded to `±MAX_HISTORY`. **Heap-owned** (~1.13 MB) — the `Searcher`/`History` lives on the search thread's stack, too small for it inline. |
| `useKillers_` / `useHistory_` / `useCounter_` / `useContHist_` / `useCaptureHist_` | `bool` | Per-signal on/off (default on); set by [`setEnabled`](#historysetenabled). |

**Methods:** [`clear`](#historyclear), [`setEnabled`](#historysetenabled),
[`quietScore`](#historyquietscore), [`killer`](#historykiller),
[`counter`](#historycounter), [`continuationScore`](#historycontinuationscore),
[`quietHistory`](#historyquiethistory), [`captureScore`](#historycapturescore),
[`updateQuietCutoff`](#historyupdatequietcutoff),
[`updateCaptureCutoff`](#historyupdatecapturecutoff) (public); `counterIndex`,
`victimIndex`, `updateContinuation` (private helpers).

**Used by:** [`Searcher`](search.hpp.md#class-searcher) (owns one; its move loop reads
[`quietHistory`](#historyquiethistory) for history pruning / LMR),
[`orderMoves`](movepick.hpp.md#ordermoves) (reads the queries).

## Functions

### `History::clear`

Reset every table to empty. Called by the constructor; available for reuse/tests.
`butterfly_` and the `continuation_` table are zeroed; every killer and countermove slot
is set to `Move::NO_MOVE`. Does **not** touch the `setEnabled` toggles (they are
configuration, not per-search state). The constructor allocates `continuation_` (via
`std::make_unique`) before calling `clear`.

### `History::setEnabled`

Enable or disable each signal independently: `setEnabled(killers, history, countermove,
contHist, captureHist)`. A disabled signal's query returns "empty" (`quietScore` → `0`,
`killer` → `NO_MOVE`, `counter` → `NO_MOVE`, `continuationScore` → `0`, `captureScore` →
`0`) so it has no ordering effect, while the (cheap) updates keep running (the
continuation and capture *updates* are skipped when disabled). Set once per search from
[`Tunables`](search.hpp.md#struct-tunables) at the top of
[`Searcher::think`](search.hpp.md#searcherthink), driven by the `UseKillers` /
`UseHistory` / `UseCountermove` / `UseContHist` / `UseCapHist` UCI options. Exists to
**A/B-isolate** each signal's Elo.

**Warning:** `quietHistory` is the sum of the butterfly and continuation queries, so
turning **both** `UseHistory` and `UseContHist` off makes it identically `0` — which
silently disables the search's history pruning and history-modulated LMR too. Ablate
those with their own toggles (`UseHistPrune` / `UseHistLmr`), and when ablating
`UseHistory` itself, turn them off as well so the measurement stays clean.

### `History::quietScore`

Butterfly-history score of a quiet `move` for side `stm`, in
`[-MAX_HISTORY, MAX_HISTORY]`. Higher means "this move cut more often here before."
Returns `0` when the history signal is disabled ([`setEnabled`](#historysetenabled)).

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

### `History::continuationScore`

Continuation-history score for playing `(movedPiece, to)` given the recent-move context
`ctx`, summed over the prior plies present in `ctx` (1 and 2 back). In
`[-CONT_PLIES * MAX_HISTORY, CONT_PLIES * MAX_HISTORY]`; `0` when continuation history is
disabled ([`setEnabled`](#historysetenabled)) or `ctx` has no prior move. `movedPiece` is
a colour+type index (0–11), `to` a square (0–63). Hot path — up to two array reads.

**Parameters:** `ctx` (`const ContHistContext&`), `movedPiece` (`int`), `to` (`int`).
**Returns:** `int`. Consumed by [`orderMoves`](movepick.hpp.md#ordermoves) to fold the
continuation score into a quiet's ordering key.

### `History::quietHistory`

The combined quiet-ordering score of `move` for `stm` under the recent-move context
`ctx`: [`quietScore`](#historyquietscore) plus
[`continuationScore`](#historycontinuationscore), i.e. **exactly** the sum
[`orderMoves`](movepick.hpp.md#ordermoves) ranks quiets by. In
`[-(1 + CONT_PLIES) · MAX_HISTORY, +(1 + CONT_PLIES) · MAX_HISTORY]` (`±49152`), and `0`
when both signals are disabled.

**Parameters:** `stm` (`Color`), `ctx` (`const ContHistContext&`), `move` (`Move`),
`movedPiece` (`int`, `move`'s colour+type index 0–11 — the caller already has it, so the
query does not re-read the board).
**Returns:** `int`. Hot path — up to three array reads.

One definition, two consumers: [`orderMoves`](movepick.hpp.md#ordermoves) scores quiets
with it, and [`Searcher::search`](search.hpp.md#searchersearch)'s move loop reads it per
quiet to drive **history pruning** and the **history-modulated LMR** adjustment (Phase
1d) — so pruning, reduction, and ordering can never disagree about how good a quiet has
been.

### `History::captureScore`

Capture-history score of `move` — how often taking **this victim on this square with this
piece** has produced a beta cutoff — in `[-MAX_HISTORY, MAX_HISTORY]`; `0` when capture
history is disabled or the bucket is untouched. `move` must be a capture.

**Parameters:** `board` (`const Board&`, the position with `move` unmade), `move`
(`Move`).
**Returns:** `int`. Hot path — one array read (plus the victim lookup).

The bucket is `(moving piece 0–11, target square 0–63, victim type 0–5)`; an en-passant
capture keys on `PAWN` since its target square is empty. Note the **from**-square is not
part of the key: two different attackers of the same type taking the same victim on the
same square share a bucket, which is the intended generalisation.

**Used by:** [`orderMoves`](movepick.hpp.md#ordermoves), which folds it (divided down) into
the MVV-LVA score of both the good- and bad-capture tiers.

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
| `ctx` | `const ContHistContext&` | Recent-move context keying the continuation update. |

**Effects:** rewards `cutoff` by `historyBonus(depth) = min(HIST_BONUS_CAP,
HIST_BONUS_MULT · depth²)` in **both** the butterfly table (keyed by `from`/`to`) and the
continuation table (keyed by `ctx`'s prior moves × `cutoff`'s piece/`to`); penalises every
move in `quietsTried` by the same magnitude (the **malus**) in both tables; pushes
`cutoff` into the killer slots (newest in slot 0, no duplicate); and stores `cutoff` as the
countermove to `prevMove`. The private `updateContinuation` helper applies the bonus to
each prior ply present in `ctx`; it runs only when continuation history is enabled.

### `History::updateCaptureCutoff`

Record a **capture** beta-cutoff: reward `cutoff` and penalise every capture in
`capturesTried` that was searched first and failed to cut, with the same
`historyBonus(depth)` magnitude and the same gravity update as the quiet tables. A no-op
when capture history is disabled.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | The node position, every move unmade (the caller updates after `unmakeMove`). |
| `depth` | `int` | Remaining depth; drives the bonus magnitude. |
| `cutoff` | `Move` | The capture that caused the fail-high. |
| `capturesTried` | `const Move*` | Captures searched **before** `cutoff` that failed to cut. |
| `nTried` | `int` | Length of `capturesTried` (may be `0`, in which case the pointer is unused). |

**Side Effects:** writes `1 + nTried` capture-history entries.

**Warnings:** killers, countermoves, butterfly and continuation history are deliberately
**not** touched — those tiers exist to order quiets, and feeding captures into them would
corrupt both. Only main-search cutoffs update the table; q-search orders captures with
[`orderCaptures`](movepick.hpp.md#ordercaptures) (plain MVV-LVA) and neither reads nor
writes capture history.

**Warnings (quiet tables):** both the butterfly and continuation updates are **gravity** updates —
`entry += bonus − entry·|bonus|/MAX_HISTORY` — which drive the entry toward the bound
while damping as it approaches, so `|entry|` never exceeds `MAX_HISTORY` (with the bonus
itself clamped to that range). This bounds each entry inside its `int16_t` slot **and**
inside movepick's quiet score band. Only quiet cutoffs reach this update; a capture
cutoff goes to [`updateCaptureCutoff`](#historyupdatecapturecutoff) instead.
