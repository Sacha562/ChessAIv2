# Module: `search`

The search core: iterative deepening over a fail-soft, full-window alpha-beta
negamax with a [transposition table](tt.hpp.md) and soft/hard time management. It
walks the game tree, calls [`evaluate`](eval.hpp.md#evaluate) at leaves, probes and
stores TT entries, streams UCI `info` lines, and returns the best root move. Driven
by [uci](uci.hpp.md) (for live play) and [bench](bench.hpp.md) (for the
deterministic benchmark).

The evaluation is still static material only. Quiescence + SEE, PVS, the rest of the
move-ordering stack, and the pruning stack arrive later in Phases 1a–1b (see
[PLAN.md](../PLAN.md) Parts 2–4). **Phase 1a step 2** added the soft/hard time
manager; **step 3** added the [`TranspositionTable`](tt.hpp.md) probe/cutoff, TT-move
ordering, and store described under [`search`](#searchersearch).

## Source Files

- **Header (interface):** [search.hpp](search.hpp)
- **Source (implementation):** [search.cpp](search.cpp)

## Namespace

- Public API (`Limits`, `TimeConfig`, `Searcher`) in namespace `engine`.
- `MOVE_OVERHEAD_MS`, `TIME_CHECK_MASK`, and `scoreToUci` live in an anonymous
  namespace in `search.cpp` — internal linkage.

## Objects / Interfaces

### `struct Limits`

Everything a UCI `go` command can specify. Populated by
[`Engine::handleGo`](uci.hpp.md#enginehandlego) and consumed by
[`Searcher::think`](#searcherthink).

| Field | Type | Description |
|-------|------|-------------|
| `depth` | `int` | Fixed depth cap in plies; `0` = no depth cap. |
| `movetime` | `int64_t` | Exact ms to spend on this move; `0` = none. |
| `nodes` | `int64_t` | Node cap; `0` = none. |
| `wtime` / `btime` | `int64_t` | Ms remaining on White's / Black's clock. |
| `winc` / `binc` | `int64_t` | Increment (ms) per move for White / Black. |
| `movestogo` | `int` | Moves until the next time control; `0` = sudden death (a default of 30 is assumed). |
| `infinite` | `bool` | Search until an explicit `stop`. |

**Used by:** [`Searcher::think`](#searcherthink), [bench](bench.hpp.md#run)

### `struct TimeConfig`

The tunable knobs of the soft/hard time manager, held by a `Searcher` and applied
by [`setupTiming`](#searchersetuptiming). Held by [uci](uci.hpp.md) and passed to
each `Searcher` so the two `TimePermille` UCI options can A/B-tune them by self-play
without a rebuild. Scales are in **permille** (parts per 1000) of the base per-move
slice `base = remaining/movestogo + inc/2`.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `softPermille` | `int` | `600` | Soft limit = `base * softPermille / 1000`. Past it, no new ID iteration is started. |
| `hardPermille` | `int` | `2400` | Hard limit = `base * hardPermille / 1000`. Past it, the search aborts mid-iteration. |
| `assumedMovestogo` | `int` | `30` | Horizon assumed under sudden death; an explicit `movestogo` overrides it. |

**Used by:** [`Searcher`](#class-searcher) (constructor), [`Searcher::setupTiming`](#searchersetuptiming), [uci](uci.hpp.md#enginehandlesetoption)

### `class Searcher`

One search worker: one instance per search. Constructed with a shared atomic stop
flag (so the [uci](uci.hpp.md) thread can abort it mid-search), a
[`TranspositionTable&`](tt.hpp.md#class-transpositiontable) it shares with the owner
(so results persist across moves and, later, across threads), and an optional
[`TimeConfig`](#struct-timeconfig) (defaulted for untimed callers like
[bench](bench.hpp.md)).

| Field | Type | Description |
|-------|------|-------------|
| `stop_` | `std::atomic<bool>&` | Shared abort flag; set by the UCI thread to stop the search. |
| `tt_` | `TranspositionTable&` | Shared transposition table (owned by the caller; outlives the searcher). |
| `tc_` | `TimeConfig` | Time-management tunables for this search. |
| `nodes_` | `uint64_t` | Nodes visited in the current search. |
| `start_` | `std::chrono::steady_clock::time_point` | Search start timestamp. |
| `softLimitMs_` | `int64_t` | Soft budget in ms — no new depth is opened past it (`INT64_MAX` if untimed). |
| `hardLimitMs_` | `int64_t` | Hard budget in ms — the search aborts past it (`INT64_MAX` if untimed). |
| `nodeLimit_` | `int64_t` | Node cap; `0` = none. |
| `useTime_` | `bool` | Whether a wall-clock budget applies. |
| `timeUp_` | `bool` | Set once the search must abort; unwinds the recursion. |
| `rootBest_` | `Move` | Best move of the in-progress iteration. |
| `rootBestCompleted_` | `Move` | Best move of the last **completed** iteration (the one actually played). |

**Methods:** [`Searcher::think`](#searcherthink), [`Searcher::nodes`](#searchernodes) (public); [`Searcher::search`](#searchersearch), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::checkStop`](#searchercheckstop), [`Searcher::elapsedMs`](#searcherelapsedms) (private).

**Used by:** [uci](uci.hpp.md), [bench](bench.hpp.md)

## Functions

### `Searcher::think`

Run the full search: configure limits, then iterative-deepen until the depth cap,
time, or a forced mate stops it. Optionally prints `info` lines each iteration and
a final `bestmove`.

**When to use:** once per move, from the search thread.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `Board` | Position to search. Taken **by value** — `think` searches on its own copy, so the caller's board is untouched. |
| `limits` | `const Limits&` | Depth/time/node bounds for this move. |
| `printBest` | `bool` | Emit the final `bestmove` line (default `true`). |
| `printInfo` | `bool` | Emit per-iteration `info` lines (default `true`). |

**Returns:** `Move` — the best move found (`rootBestCompleted_`). Returns
`Move::NO_MOVE` and prints `bestmove 0000` when the position has no legal moves.

**Side Effects:** writes to `std::cout` (`info` / `bestmove`, including a
`hashfull` field from [`tt_.hashfull()`](tt.hpp.md#transpositiontablehashfull));
calls [`tt_.newSearch()`](tt.hpp.md#transpositiontablenewsearch) once to advance the
TT generation; resets and updates all timing/node/root-move members. Reads the
shared `stop_` flag.

**Warnings:** the first legal move is stored as a fallback **before** searching, so
`think` never returns a null move in a non-terminal position even if aborted at
depth 1. An aborted iteration is discarded (the previous completed iteration's move
stands). Soft stop: once elapsed time reaches [`softLimitMs_`](#class-searcher) no
new depth is started; the current depth may still run until the hard limit.

### `Searcher::nodes`

Return `uint64_t` — nodes visited in the current/last search. Read by
[bench](bench.hpp.md#run) to accumulate the benchmark node total. Trivial getter.

### `Searcher::search` (private)

The recursive fail-soft negamax (returns `best`, full window). Order of operations:
draw check → **TT probe** → leaf/movegen → **TT-move-first ordering** → move loop →
**TT store**.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `Board&` | Current position; mutated via make/unmake and restored before return (in/out). |
| `depth` | `int` | Remaining depth in plies; `<= 0` returns the static eval. |
| `alpha` | `Value` | Lower bound of the search window. |
| `beta` | `Value` | Upper bound of the search window. |
| `ply` | `int` | Distance from the root (for mate scoring and root-move tracking). |

**Returns:** [`Value`](types.hpp.md#using-value--int) — the negamax score of
`board`. `mated_in(ply)` at checkmate, `VALUE_DRAW` at stalemate/draw, `VALUE_ZERO`
when aborting (`timeUp_`).

**Transposition table use:**
- **Probe** at [`board.hash()`](../include/chess.hpp). A hit yields the stored move
  (used for ordering) and, when `ply > 0`, `entry.depth >= depth`, and the fifty-move
  clock is low (`halfMoveClock() < 90`), an immediate cutoff if the bound qualifies:
  `EXACT` always, `LOWER` when `ttv >= beta`, `UPPER` when `ttv <= alpha`, where
  `ttv = valueFromTT(entry.value, ply)`. **Never cuts at the root** so a move is
  always produced.
- **Ordering**: the TT move (if any) is swapped to the front of the move list — the
  only ordering signal until Phase 1a step 6.
- **Store** after the loop (unless aborted): `depth`, `valueToTT(best, ply)`, the
  best move, and the bound — `LOWER` if `best >= beta`, `EXACT` if `best > alphaOrig`,
  else `UPPER`. `alphaOrig` is captured before the loop mutates `alpha`.
- **Prefetch**: `tt_.prefetch(board.zobristAfter(m))` before each `makeMove`.

**Side Effects:** mutates `board` transiently; probes/stores `tt_`; updates `nodes_`
and, at `ply == 0`, `rootBest_`; periodically calls `checkStop`.

**Warnings:** draw detection is skipped at the root (`ply > 0` guard) so a move is
always returned. Uses `isRepetition(1)` (twofold) as the in-search draw rule — the
library's default is threefold. On abort, partial results are discarded and **not**
stored in the TT. A stale/illegal TT move from a key collision is harmless: it is
only searched if it matches a generated legal move (the swap is a no-op otherwise).
Mate scores are rebased through the TT via [`valueToTT`/`valueFromTT`](tt.hpp.md#valuetott--valuefromtt).

### `Searcher::setupTiming` (private)

Derive `nodeLimit_`, `useTime_`, `softLimitMs_`, and `hardLimitMs_` for one `go`,
from the [`Limits`](#struct-limits), the side to move, and [`tc_`](#struct-timeconfig).
Called once at the top of [`think`](#searcherthink).

- **`movetime`**: soft = hard = `movetime − MOVE_OVERHEAD_MS` (spend it exactly).
- **depth / nodes / infinite**: no clock (`useTime_` stays false; runs untimed).
- **clock (`wtime`/`btime`…)**: `base = remaining/movestogo + inc/2` (movestogo
  defaults to `tc_.assumedMovestogo`), then soft/hard scale off `base` by permille.
  The hard limit is capped at half the remaining clock (minus overhead) so the
  engine never flags, and soft is clamped to ≤ hard.

### `Searcher::checkStop` (private)

Return `true` and set `timeUp_` if the search must stop: the shared `stop_` flag is
set, the node cap is reached, or the **hard** time limit is exhausted. Called every
2048 nodes (`TIME_CHECK_MASK`) plus at the top of each iteration.

### `Searcher::elapsedMs` (private)

Return `int64_t` — milliseconds since `start_`. Trivial.
