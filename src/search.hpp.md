# Module: `search`

The search core: iterative deepening over a plain full-window alpha-beta negamax.
It walks the game tree, calls [`evaluate`](eval.hpp.md#evaluate) at leaves, streams
UCI `info` lines, and returns the best root move. Driven by [uci](uci.hpp.md) (for
live play) and [bench](bench.hpp.md) (for the deterministic benchmark).

Phase 0 is deliberately minimal — full-window alpha-beta, static leaf eval, basic
time management. Transposition table, quiescence + SEE, PVS, move ordering, and the
pruning stack arrive in Phases 1a–1b (see [PLAN.md](../PLAN.md) Parts 2–4).

## Source Files

- **Header (interface):** [search.hpp](search.hpp)
- **Source (implementation):** [search.cpp](search.cpp)

## Namespace

- Public API (`Limits`, `Searcher`) in namespace `engine`.
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

### `class Searcher`

One search worker: one instance per search. Constructed with a shared atomic stop
flag so the [uci](uci.hpp.md) thread can abort it mid-search.

| Field | Type | Description |
|-------|------|-------------|
| `stop_` | `std::atomic<bool>&` | Shared abort flag; set by the UCI thread to stop the search. |
| `nodes_` | `uint64_t` | Nodes visited in the current search. |
| `start_` | `std::chrono::steady_clock::time_point` | Search start timestamp. |
| `allocatedMs_` | `int64_t` | Hard time budget in ms (`INT64_MAX` if untimed). |
| `nodeLimit_` | `int64_t` | Node cap; `0` = none. |
| `useTime_` | `bool` | Whether a wall-clock budget applies. |
| `timeUp_` | `bool` | Set once the search must abort; unwinds the recursion. |
| `rootBest_` | `Move` | Best move of the in-progress iteration. |
| `rootBestCompleted_` | `Move` | Best move of the last **completed** iteration (the one actually played). |

**Methods:** [`Searcher::think`](#searcherthink), [`Searcher::nodes`](#searchernodes) (public); [`Searcher::search`](#searchersearch), [`Searcher::checkStop`](#searchercheckstop), [`Searcher::elapsedMs`](#searcherelapsedms) (private).

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

**Side Effects:** writes to `std::cout` (`info` / `bestmove`); resets and updates
all timing/node/root-move members. Reads the shared `stop_` flag.

**Warnings:** the first legal move is stored as a fallback **before** searching, so
`think` never returns a null move in a non-terminal position even if aborted at
depth 1. An aborted iteration is discarded (the previous completed iteration's move
stands). Soft stop: a new depth is not started once past half the time budget.

### `Searcher::nodes`

Return `uint64_t` — nodes visited in the current/last search. Read by
[bench](bench.hpp.md#run) to accumulate the benchmark node total. Trivial getter.

### `Searcher::search` (private)

The recursive negamax. Fail-soft in spirit (returns `best`), full-window in Phase 0.

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

**Side Effects:** mutates `board` transiently; updates `nodes_` and, at `ply == 0`,
`rootBest_`; periodically calls `checkStop`.

**Warnings:** draw detection is skipped at the root (`ply > 0` guard) so a move is
always returned. Uses `isRepetition(1)` (twofold) as the in-search draw rule — the
library's default is threefold. On abort, partial results are discarded by the
caller.

### `Searcher::checkStop` (private)

Return `true` and set `timeUp_` if the search must stop: the shared `stop_` flag is
set, the node cap is reached, or the time budget is exhausted. Called every 2048
nodes (`TIME_CHECK_MASK`) plus at the top of each iteration.

### `Searcher::elapsedMs` (private)

Return `int64_t` — milliseconds since `start_`. Trivial.
