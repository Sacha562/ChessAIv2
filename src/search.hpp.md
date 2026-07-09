# Module: `search`

The search core: iterative deepening over a fail-soft **Principal Variation Search**
(a null-window scout + re-search refinement of alpha-beta negamax) with a
[transposition table](tt.hpp.md) and soft/hard time management. It walks the game
tree, calls [`evaluate`](eval.hpp.md#evaluate) at leaves, probes and stores TT
entries, streams UCI `info` lines, and returns the best root move. Driven by
[uci](uci.hpp.md) (for live play) and [bench](bench.hpp.md) (for the deterministic
benchmark).

The evaluation is still static material only. The forward-pruning stack (null-move,
LMR, futility/LMP) arrives later in Phase 1b (see [PLAN.md](../PLAN.md) Parts 2–4).
**Phase 1a step 2** added the soft/hard time manager; **step 3** added the
[`TranspositionTable`](tt.hpp.md) probe/cutoff and store; **step 4** added
[`see`](see.hpp.md) (Static Exchange Evaluation); **step 5** added the
[`qsearch`](#searcherqsearch) quiescence layer at the leaf; **step 6** replaced the
TT-move-only ordering with full [`movepick`](movepick.hpp.md) ordering; and **step 7**
turned the full-window move loop into fail-soft PVS. **Phase 1b step 1** added the
quiet-move ordering heuristics ([`History`](history.hpp.md): killers, butterfly
history with malus, countermoves) and **Internal Iterative Reduction** (IIR). Aspiration
windows ([PLAN.md](../PLAN.md) Component 6) are the remaining search-core item.

## Source Files

- **Header (interface):** [search.hpp](search.hpp)
- **Source (implementation):** [search.cpp](search.cpp)

## Namespace

- Public API (`Limits`, `Tunables`, `Searcher`) in namespace `engine`.
- `MOVE_OVERHEAD_MS`, `TIME_CHECK_MASK`, `IIR_MIN_DEPTH`, and `scoreToUci` live in an
  anonymous namespace in `search.cpp` — internal linkage. (The former `DELTA_MARGIN` /
  `ENDGAME_PIECES` q-search constants are now runtime-tunable fields of
  [`Tunables`](#struct-tunables).)

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

### `struct Tunables`

The self-play-tunable engine knobs, held by a `Searcher` and copied in from
[uci](uci.hpp.md) so every knob can be A/B-tuned by self-play (SPSA) through UCI spin
options without a rebuild. Grouped by subsystem: time management (applied by
[`setupTiming`](#searchersetuptiming)), evaluation, and q-search delta pruning. Time
scales are in **permille** (parts per 1000) of the base per-move slice
`base = remaining/movestogo + inc/2`.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `softPermille` | `int` | `600` | Soft limit = `base * softPermille / 1000`. Past it, no new ID iteration is started. |
| `hardPermille` | `int` | `2400` | Hard limit = `base * hardPermille / 1000`. Past it, the search aborts mid-iteration. |
| `assumedMovestogo` | `int` | `30` | Horizon assumed under sudden death; an explicit `movestogo` overrides it. Divides the clock, so it must stay `>= 1`. |
| `tempo` | `Value` | `9` | Side-to-move bonus (cp) passed to [`evaluate`](eval.hpp.md#evaluate). |
| `deltaMargin` | `Value` | `203` | Q-search delta-pruning safety cushion (cp). |
| `endgamePieces` | `int` | `7` | Total piece count at/below which delta pruning switches off. |

**Used by:** [`Searcher`](#class-searcher) (constructor), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::qsearch`](#searcherqsearch), [`evaluate`](eval.hpp.md#evaluate), [uci](uci.hpp.md#enginehandlesetoption)

### `class Searcher`

One search worker: one instance per search. Constructed with a shared atomic stop
flag (so the [uci](uci.hpp.md) thread can abort it mid-search), a
[`TranspositionTable&`](tt.hpp.md#class-transpositiontable) it shares with the owner
(so results persist across moves and, later, across threads), and an optional
[`Tunables`](#struct-tunables) (defaulted for callers like [bench](bench.hpp.md) that
just want the defaults).

| Field | Type | Description |
|-------|------|-------------|
| `stop_` | `std::atomic<bool>&` | Shared abort flag; set by the UCI thread to stop the search. |
| `tt_` | `TranspositionTable&` | Shared transposition table (owned by the caller; outlives the searcher). |
| `tp_` | `Tunables` | Self-play-tunable knobs (time, eval tempo, q-search delta) for this search. |
| `nodes_` | `uint64_t` | Nodes visited in the current search. |
| `history_` | [`History`](history.hpp.md) | Quiet-move ordering heuristics (killers, butterfly history, countermoves) for this search. |
| `start_` | `std::chrono::steady_clock::time_point` | Search start timestamp. |
| `softLimitMs_` | `int64_t` | Soft budget in ms — no new depth is opened past it (`INT64_MAX` if untimed). |
| `hardLimitMs_` | `int64_t` | Hard budget in ms — the search aborts past it (`INT64_MAX` if untimed). |
| `nodeLimit_` | `int64_t` | Node cap; `0` = none. |
| `useTime_` | `bool` | Whether a wall-clock budget applies. |
| `timeUp_` | `bool` | Set once the search must abort; unwinds the recursion. |
| `rootBest_` | `Move` | Best move of the in-progress iteration. |
| `rootBestCompleted_` | `Move` | Best move of the last **completed** iteration (the one actually played). |

**Methods:** [`Searcher::think`](#searcherthink), [`Searcher::nodes`](#searchernodes) (public); [`Searcher::search`](#searchersearch), [`Searcher::qsearch`](#searcherqsearch), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::checkStop`](#searchercheckstop), [`Searcher::elapsedMs`](#searcherelapsedms) (private).

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
new depth is started; the current depth may still run until the hard limit. The
iterative-deepening cap is clamped to `MAX_PLY - 1` even when `go depth N` requests a
larger `N`, so a node's `ply` can never overrun the fixed per-ply tables (the
[`History`](history.hpp.md) killers) it indexes.

### `Searcher::nodes`

Return `uint64_t` — nodes visited in the current/last search. Read by
[bench](bench.hpp.md#run) to accumulate the benchmark node total. Trivial getter.

### `Searcher::search` (private)

The recursive fail-soft negamax, refined with **Principal Variation Search** (returns
`best`). Order of operations: draw check → **leaf → [`qsearch`](#searcherqsearch)**
(when `depth <= 0`) → **TT probe** → **IIR** → movegen →
**[`orderMoves`](movepick.hpp.md#ordermoves)** → **PVS move loop** (with quiet-cutoff
heuristic updates) → **TT store**.

**Internal Iterative Reduction (IIR):** when a node has real depth (`>= IIR_MIN_DEPTH`,
currently 4) but the TT probe returned **no move**, ordering has nothing to lead with,
so a full-depth search is likely wasted behind a poor first move. `depth` is reduced by
one ply before movegen; the cheaper search leaves a TT move behind that a later
re-visit (or the same iterative-deepening line one ply up) can order on. The reduced
depth is also what is stored in the TT, correctly reflecting the work actually done.

**Quiet-cutoff learning:** on a fail-high (`alpha >= beta`) caused by a **quiet** move,
[`history_.updateQuietCutoff`](history.hpp.md#historyupdatequietcutoff) rewards that move,
penalises the quiet moves tried before it (tracked in a per-node `quietsTried` stack
array), and updates killers and the countermove for `prevMove`. Capture cutoffs do not
touch history. These signals feed the next node's `orderMoves`.

**Principal Variation Search (PVS):** the first (best-ordered) move is searched on the
full `[alpha, beta]` window to establish the principal variation. Every later move is
first probed with a **null-window scout** `(alpha, alpha+1)`, which produces far more
beta-cutoffs and so refutes an inferior move cheaply. Only when a scout both **raises
alpha and stays below beta** (`alpha < score < beta`) was the ordering wrong for that
move, and it is **re-searched** on the full window to obtain its exact score. The
`score < beta` half of the guard also makes PVS a no-op at an already-null window
(`beta == alpha+1`, i.e. a scout node one level up), where the scout *is* the full
window — no redundant re-search is issued. PVS relies on the move ordering being good
(the TT move / SEE-split from step 6) and is a node-count win: its bench signature is
lower than step 6's. It does not change the search **result** — the root score, best
move, and PV are identical to the plain full-window search. It is *not* strictly
value-identical at every interior node, though: because the search is fail-soft, when a
later move's null-window scout fails high (`score >= beta`) the re-search is skipped, so
the node may return and TT-store a looser — but still sound — lower bound than a
full-window search of that move would. That never flips a cutoff or a move choice, only
the numeric bound; the root (searched with `beta = +VALUE_INFINITE`) always re-searches
an alpha-raising move, so the reported score stays exact and deterministic.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `Board&` | Current position; mutated via make/unmake and restored before return (in/out). |
| `depth` | `int` | Remaining depth in plies; `<= 0` drops into [`qsearch`](#searcherqsearch). |
| `alpha` | `Value` | Lower bound of the search window. |
| `beta` | `Value` | Upper bound of the search window. |
| `ply` | `int` | Distance from the root (for mate scoring and root-move tracking). |
| `prevMove` | `Move` | The move played at the parent node (`Move::NO_MOVE` at the root); keys the countermove and is the malus/history side-to-move context. |

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
- **Ordering**: the probed TT move is passed to
  [`orderMoves`](movepick.hpp.md#ordermoves) together with [`history_`](history.hpp.md),
  the `ply`, and `prevMove`, which floats the TT move first and then sorts the rest
  (good captures by SEE+MVV-LVA, killers, countermove, history-scored quiets, losing
  captures).
- **Store** after the loop (unless aborted): `depth`, `valueToTT(best, ply)`, the
  best move, and the bound — `LOWER` if `best >= beta`, `EXACT` if `best > alphaOrig`,
  else `UPPER`. `alphaOrig` is captured before the loop mutates `alpha`.
- **Prefetch**: `tt_.prefetch(board.zobristAfter(m))` before each `makeMove`.

**Side Effects:** mutates `board` transiently; probes/stores `tt_`; updates `nodes_`,
`history_` (on quiet cutoffs), and, at `ply == 0`, `rootBest_`; periodically calls
`checkStop`.

**Warnings:** draw detection is skipped at the root (`ply > 0` guard) so a move is
always returned. Uses `isRepetition(1)` (twofold) as the in-search draw rule — the
library's default is threefold. On abort, partial results are discarded and **not**
stored in the TT. A stale/illegal TT move from a key collision is harmless: it is
only searched if it matches a generated legal move (the swap is a no-op otherwise).
Mate scores are rebased through the TT via [`valueToTT`/`valueFromTT`](tt.hpp.md#valuetott--valuefromtt).

### `Searcher::qsearch` (private)

Quiescence search: the leaf layer reached when the main search hits `depth <= 0`. It
keeps resolving **captures** (and, when in check, **all evasions**) until the position
is quiet, so the static eval is never read in the middle of a capture sequence (the
horizon effect). Fail-soft, full window; it keeps its own node/time accounting but
uses **no TT** of its own.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `Board&` | Current position; mutated via make/unmake and restored (in/out). |
| `alpha` | `Value` | Lower bound of the window. |
| `beta` | `Value` | Upper bound of the window. |
| `ply` | `int` | Distance from the root (mate scoring, depth guard). |

**Returns:** [`Value`](types.hpp.md#using-value--int) — the fail-soft quiescence score.
`mated_in(ply)` when in check with no evasions; `VALUE_DRAW` on a draw; `VALUE_ZERO`
when aborting (`timeUp_`).

**Behavior:**
- **Stand-pat**: when *not* in check, [`evaluate`](eval.hpp.md#evaluate)`(board, tp_.tempo)`
  seeds `best` as a lower bound — return immediately on `best >= beta`, else raise `alpha`. When
  **in check**, standing pat is forbidden: `best` starts at `-∞` and every legal
  evasion is searched (`orderMoves`, ordered with the shared `history_` killers/history
  but no TT/countermove context), so a genuine checkmate scores `mated_in(ply)`.
- **Move set**: not in check → captures / capture-promotions only
  (`legalmoves<CAPTURE>`), [`orderCaptures`](movepick.hpp.md#ordercaptures) by MVV-LVA.
- **SEE pruning**: a capture with [`seeGE`](see.hpp.md#seege)`(m, 0) == false` (a losing
  exchange) is skipped — the single biggest q-search node reducer.
- **Delta pruning**: skip a capture when `standPat + pieceValue(victim) + tp_.deltaMargin
  <= alpha` — it cannot lift the score to `alpha`. Disabled when in check, for
  promotions, and in late endgames (`board.occ().count() <= tp_.endgamePieces`), where
  material swings decide the game.
- **Depth guard**: returns the static eval at `ply >= MAX_PLY - 1` so a long checking
  sequence cannot overflow the ply budget.

**Side Effects:** mutates `board` transiently; updates `nodes_`; periodically calls
`checkStop`. Reads (does not write) the TT via nothing — q-search does not probe or
store.

**Warnings:** the q-search recurses on captures (and check evasions) only, never on
quiet checks, so it terminates as material is consumed. Non-capturing (quiet)
promotions are searched only insofar as `legalmoves<CAPTURE>` yields them; a purely
quiet queen promotion may be deferred to the main search. **Stalemate is not detected
when not in check**: an empty capture list means "no captures", not "no moves", so a
stalemated node returns its stand-pat eval instead of `VALUE_DRAW` — the standard
quiescence limitation (enumerating quiets would defeat q-search's purpose); the main
search still scores stalemate correctly at `depth > 0`. There is no TT cutoff here, so
q-search results are recomputed each visit (a future optimization).

### `Searcher::setupTiming` (private)

Derive `nodeLimit_`, `useTime_`, `softLimitMs_`, and `hardLimitMs_` for one `go`,
from the [`Limits`](#struct-limits), the side to move, and [`tp_`](#struct-tunables).
Called once at the top of [`think`](#searcherthink).

- **`movetime`**: soft = hard = `movetime − MOVE_OVERHEAD_MS` (spend it exactly).
- **depth / nodes / infinite**: no clock (`useTime_` stays false; runs untimed).
- **clock (`wtime`/`btime`…)**: `base = remaining/movestogo + inc/2` (movestogo
  defaults to `tp_.assumedMovestogo`), then soft/hard scale off `base` by permille.
  The hard limit is capped at half the remaining clock (minus overhead) so the
  engine never flags, and soft is clamped to ≤ hard.

### `Searcher::checkStop` (private)

Return `true` and set `timeUp_` if the search must stop: the shared `stop_` flag is
set, the node cap is reached, or the **hard** time limit is exhausted. Called every
2048 nodes (`TIME_CHECK_MASK`) plus at the top of each iteration.

### `Searcher::elapsedMs` (private)

Return `int64_t` — milliseconds since `start_`. Trivial.
