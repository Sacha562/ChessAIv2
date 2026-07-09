# Module: `search`

The search core: iterative deepening over a fail-soft **Principal Variation Search**
(a null-window scout + re-search refinement of alpha-beta negamax) with a
[transposition table](tt.hpp.md) and soft/hard time management. It walks the game
tree, calls [`evaluate`](eval.hpp.md#evaluate) at leaves, probes and stores TT
entries, streams UCI `info` lines, and returns the best root move. Driven by
[uci](uci.hpp.md) (for live play) and [bench](bench.hpp.md) (for the deterministic
benchmark).

The evaluation is still static material only. Phase 1a built the search *core* (TT,
SEE, qsearch, movepick ordering, PVS, soft/hard time management). **Phase 1b** then
added the selective-search layers on top (see [PLAN.md](../PLAN.md) Parts 3–4):

- **step 1** — the quiet-move ordering heuristics ([`History`](history.hpp.md): killers,
  butterfly history with malus, countermoves) and **Internal Iterative Reduction** (IIR);
- **the rest of Phase 1b** — the forward-pruning / reduction / extension stack, all in
  [`search`](#searchersearch): a per-ply **static eval** with the `improving` trend,
  **reverse futility pruning** (RFP), **null-move pruning** (NMP), **futility pruning**
  and **late-move (move-count) pruning** (LMP) of quiets, **late move reductions** (LMR),
  **check extensions**, and **aspiration windows** ([`aspirationSearch`](#searcheraspirationsearch))
  at the root.

Each selective layer is behind a [`Tunables`](#struct-tunables) toggle so it can be
individually A/B-isolated — the discipline that caught step-1's history/IIR regressions.
Everything above still sits on the fail-soft **Principal Variation Search** negamax with
a [transposition table](tt.hpp.md).

## Source Files

- **Header (interface):** [search.hpp](search.hpp)
- **Source (implementation):** [search.cpp](search.cpp)

## Namespace

- Public API (`Limits`, `Tunables`, `Searcher`) in namespace `engine`.
- `MOVE_OVERHEAD_MS`, `TIME_CHECK_MASK`, `scoreToUci`, and the selective-search knobs —
  `IIR_MIN_DEPTH`, `TT_CUTOFF_MAX_HALFMOVE`, the `NMP_*`, `RFP_*`, `FUT_*`,
  `LMP_MAX_DEPTH`, `LMR_*`, and `ASPIRATION_MIN_DEPTH` constants, plus the helpers
  `lmrReduction` (a precomputed
  `ln·ln` reduction table, lazily built once), `lmpCount`, and `hasNonPawnMaterial` —
  live in an anonymous namespace in `search.cpp` (internal linkage). The margins are
  first-cut and SPSA-tunable later. (The former `DELTA_MARGIN` / `ENDGAME_PIECES`
  q-search constants are now runtime-tunable fields of [`Tunables`](#struct-tunables).)

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
| `useKillers` | `bool` | `true` | Enable the killer-move ordering signal (`UseKillers`). |
| `useHistory` | `bool` | `true` | Enable the butterfly-history ordering signal (`UseHistory`). |
| `useCountermove` | `bool` | `true` | Enable the countermove ordering signal (`UseCountermove`). |
| `useIir` | `bool` | `true` | Enable Internal Iterative Reduction (`UseIIR`). |
| `useNmp` | `bool` | `true` | Enable null-move pruning (`UseNMP`). |
| `useRfp` | `bool` | `true` | Enable reverse futility / static null-move pruning (`UseRFP`). |
| `useFutility` | `bool` | `true` | Enable futility pruning of quiets near the horizon (`UseFutility`). |
| `useLmp` | `bool` | `true` | Enable late-move (move-count) pruning (`UseLMP`). |
| `useLmr` | `bool` | `true` | Enable late move reductions (`UseLMR`). |
| `useCheckExt` | `bool` | `true` | Enable check extensions (`UseCheckExt`). |
| `useAspiration` | `bool` | `true` | Enable root aspiration windows (`UseAspiration`). |
| `aspirationDelta` | `Value` | `15` | Initial half-window (cp) around the previous score (`AspirationDelta`). |

The `use*` toggles exist to **A/B-isolate** each Phase 1b signal's Elo (flip one on/off,
run an SPRT). `useKillers` / `useHistory` / `useCountermove` are applied via
[`History::setEnabled`](history.hpp.md#historysetenabled) (called at the top of
[`think`](#searcherthink)); `useIir` / `useNmp` / `useRfp` / `useFutility` / `useLmp` /
`useLmr` / `useCheckExt` each gate their step inside [`search`](#searchersearch); and
`useAspiration` / `aspirationDelta` drive
[`aspirationSearch`](#searcheraspirationsearch). With every selective toggle off the
search reproduces the plain PVS + step-1-ordering engine.

**Default rationale (A/B @ 8+0.08 vs the same binary):** all four are **on**. Killers
**+44 Elo** and countermove **~0**. Butterfly **history** and **IIR** both *inverted*
once the pruning stack existed to consume them: history **−23 → +247** and IIR
**−36 → +16** (95% CI `[+4, +28]`), because LMP/LMR prune/reduce late-ordered quiets, so
good ordering and a hash-move-first search are what stop them discarding a winning move
("ordering makes pruning safe", PLAN.md Part 4). All four remain wired and UCI-tunable
for per-signal A/B isolation.

**Used by:** [`Searcher`](#class-searcher) (constructor), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::qsearch`](#searcherqsearch), [`Searcher::think`](#searcherthink), [`Searcher::search`](#searchersearch), [`evaluate`](eval.hpp.md#evaluate), [uci](uci.hpp.md#enginehandlesetoption)

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
| `staticEvals_` | `Value[MAX_PLY]` | Per-ply static eval, for RFP / futility / the `improving` trend. `VALUE_NONE` at in-check plies. |
| `start_` | `std::chrono::steady_clock::time_point` | Search start timestamp. |
| `softLimitMs_` | `int64_t` | Soft budget in ms — no new depth is opened past it (`INT64_MAX` if untimed). |
| `hardLimitMs_` | `int64_t` | Hard budget in ms — the search aborts past it (`INT64_MAX` if untimed). |
| `nodeLimit_` | `int64_t` | Node cap; `0` = none. |
| `useTime_` | `bool` | Whether a wall-clock budget applies. |
| `timeUp_` | `bool` | Set once the search must abort; unwinds the recursion. |
| `rootBest_` | `Move` | Best move of the in-progress iteration. |
| `rootBestCompleted_` | `Move` | Best move of the last **completed** iteration (the one actually played). |

**Methods:** [`Searcher::think`](#searcherthink), [`Searcher::nodes`](#searchernodes) (public); [`Searcher::search`](#searchersearch), [`Searcher::aspirationSearch`](#searcheraspirationsearch), [`Searcher::qsearch`](#searcherqsearch), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::checkStop`](#searchercheckstop), [`Searcher::elapsedMs`](#searcherelapsedms) (private).

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
new depth is started; the current depth may still run until the hard limit. The root
depth is clamped to `MAX_PLY - 1` even when `go depth N` requests a larger `N`, so the
iterative-deepening loop itself cannot run past the per-ply tables. A node's `ply` can
still exceed the root depth via **check extensions**; what keeps the
[`History`](history.hpp.md) killers and `staticEvals_` in bounds there is the
`ply >= MAX_PLY - 1` leaf guard in [`search`](#searchersearch) / [`qsearch`](#searcherqsearch)
plus the `ply + 1 < MAX_PLY - 1` gate on the extension.

### `Searcher::nodes`

Return `uint64_t` — nodes visited in the current/last search. Read by
[bench](bench.hpp.md#run) to accumulate the benchmark node total. Trivial getter.

### `Searcher::search` (private)

The recursive fail-soft negamax, refined with **Principal Variation Search** and the
full Phase 1b selective-search stack (returns `best`). Order of operations:

1. draw check → **leaf / ply guard → [`qsearch`](#searcherqsearch)** (when `depth <= 0`
   or `ply >= MAX_PLY - 1`);
2. **TT probe** (cutoff only off the PV) → **static eval** (reused from the TT when
   present; skipped in check) → **`improving`** trend;
3. **whole-node pruning** (off-PV, not in check, away from mate): **RFP** then **NMP**;
4. **IIR** (gated, default off) → movegen → **[`orderMoves`](movepick.hpp.md#ordermoves)**;
5. **move loop**: per-move quiet pruning (**LMP**, **futility**) → make → **check
   extension** → **LMR** reduction → **PVS** search/re-search → cutoff bookkeeping
   (quiet-cutoff [`history`](history.hpp.md#historyupdatequietcutoff) updates);
6. **TT store** (value, best move, bound, and the static eval).

`pvNode` is derived as `beta - alpha > 1`: the root and full-window nodes are PV; the
null-window scout nodes are not. Every forward-pruning / reduction step is restricted to
non-PV nodes (and skipped in check and near mate scores) so the principal variation is
searched exactly.

**Selective-search steps** (each gated by its [`Tunables`](#struct-tunables) toggle):

- **Static eval & `improving`** — at a non-check node the static eval is read from the
  TT (`tt.eval`) or computed by [`evaluate`](eval.hpp.md#evaluate) and cached in
  `staticEvals_[ply]`. `improving` is true when this eval exceeds the same side's eval
  two plies ago (`staticEvals_[ply-2]`) — pruning is a touch more aggressive when the
  position is *not* improving.
- **Reverse futility pruning (RFP)** — near the horizon (`depth <= RFP_MAX_DEPTH`), a
  static eval a depth-scaled margin **above** beta (`eval - RFP_MARGIN*depth >= beta`) is
  assumed to hold and `eval` is returned.
- **Null-move pruning (NMP)** — give the opponent a free move
  ([`board.makeNullMove()`](../include/chess.hpp)) and search at reduced depth
  `depth - 1 - R` (`R = NMP_BASE + depth/NMP_DIV + min((eval-beta)/NMP_EVAL_DIV,
  NMP_EVAL_MAX)`); a result still `>= beta` prunes. Guarded by `eval >= beta`,
  `depth >= NMP_MIN_DEPTH`, and **non-pawn material** for the side to move (`hasNonPawnMaterial`,
  the zugzwang guard). A returned mate score is not trusted (clamped to `beta`).
- **Futility pruning** — a quiet whose `eval + FUT_MARGIN*depth + FUT_BASE <= alpha` near
  the horizon cannot reach alpha and is skipped.
- **Late-move pruning (LMP)** — past a depth-scaled quiet count (`lmpCount`, smaller when
  not `improving`) the remaining quiets are skipped.
- **Check extension** — a move that gives check is searched one ply deeper
  (`ext = 1`), bounded by `ply + 1 < MAX_PLY - 1` so `ply` can never overrun the per-ply
  tables.
- **Late move reductions (LMR)** — late quiet, non-checking moves are searched at
  `newDepth - reduction`, where `reduction` comes from the `ln(depth)·ln(moveCount)`
  table, reduced by one on the PV and one when `improving`, clamped to keep the reduced
  depth `>= 1`. A reduced search that beats alpha is re-searched at full `newDepth`
  (null window), then at the full window if it lands in `(alpha, beta)`.

The per-move quiet pruning (futility/LMP) and reductions never touch the first move,
never fire in check or on the PV, and use `continue` (not `break`) so interleaved
captures/checks are still searched; they are also suppressed while `best` is still a
mate loss (`best <= VALUE_MATED_IN_MAX_PLY`), so a forced escape is never pruned.

**Internal Iterative Reduction (IIR):** (gated by [`tp_.useIir`](#struct-tunables)) when
a node has real depth (`>= IIR_MIN_DEPTH`, currently 4) but the TT probe returned **no
move**, ordering has nothing to lead with,
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
  (used for ordering) and its cached static eval, and — **at a non-PV node** with
  `entry.depth >= depth` and a low fifty-move clock (`halfMoveClock() <
  TT_CUTOFF_MAX_HALFMOVE`, 90) — an immediate cutoff if the bound qualifies: `EXACT` always, `LOWER` when `ttv >= beta`,
  `UPPER` when `ttv <= alpha`, where `ttv = valueFromTT(entry.value, ply)`. The
  `!pvNode` guard means the root (and every PV node) **never cuts**, so a move is always
  produced and the PV stays exact.
- **Ordering**: the probed TT move is passed to
  [`orderMoves`](movepick.hpp.md#ordermoves) together with [`history_`](history.hpp.md),
  the `ply`, and `prevMove`, which floats the TT move first and then sorts the rest
  (good captures by SEE+MVV-LVA, killers, countermove, history-scored quiets, losing
  captures).
- **Store** after the loop (unless aborted): `depth`, `valueToTT(best, ply)`, the node's
  **static eval** (`VALUE_NONE` when in check), the best move, and the bound — `LOWER` if
  `best >= beta`, `EXACT` if `best > alphaOrig`, else `UPPER`. `alphaOrig` is captured
  before the loop mutates `alpha`. The stored eval is what a later probe reuses instead
  of recomputing [`evaluate`](eval.hpp.md#evaluate).
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

### `Searcher::aspirationSearch` (private)

Runs one root iteration inside an **aspiration window** and returns its score. Called
once per iterative-deepening depth from [`think`](#searcherthink), seeded with the
previous iteration's score.

- Below `ASPIRATION_MIN_DEPTH` (5), when `useAspiration` is off, or on a mate score, it
  is a plain full-window `search(depth, -∞, +∞, 0)`.
- Otherwise it opens a narrow window `[prevScore - delta, prevScore + delta]`
  (`delta = aspirationDelta`, 15 cp) and searches. On a **fail low** (`v <= alpha`) it
  drops `alpha` (and pulls `beta` toward the midpoint); on a **fail high** (`v >= beta`)
  it raises `beta`; either way it doubles `delta` and re-searches, so only the failing
  side widens. It returns as soon as the score lands inside the window (or on `timeUp_`).

**Parameters:** `board` (`Board&`, in/out), `depth` (`int`), `prevScore` (`Value` — the
last completed iteration's score). **Returns:** [`Value`](types.hpp.md#using-value--int).

**Warnings:** a narrower window wins more cutoffs but costs a re-search on every fail;
gating to `depth >= 5` avoids the noisy shallow iterations. The final in-window search
sets [`rootBest_`](#class-searcher) correctly; an aborted re-search (`timeUp_`) is
discarded by [`think`](#searcherthink) like any incomplete iteration.

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
