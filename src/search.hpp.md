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

**Phase 1c** adds two more selective layers, each behind its own toggle:
**continuation history** (a quiet-ordering signal conditioned on the moves 1 and 2 plies
back — held in [`History`](history.hpp.md), fed by a new per-ply search stack in
[`search`](#searchersearch)) and **singular extensions** (a reduced-depth verification
search that excludes the TT move; if every alternative fails low the TT move is extended,
via a new `excluded`-move parameter on [`search`](#searchersearch)).

**Pondering** lets the engine think on the opponent's clock. `go ponder` runs an
ordinary search on the predicted position with the clock stopped; the shared
[`SearchState`](#enum-class-searchstate) flips from `Pondering` to `Running` on
`ponderhit` (the clock starts, charged with
[`ponderCreditPermille`](#struct-tunables) of the time already pondered) or to `Stopped`
on `stop`. A **triangular PV table** ([`updatePv`](#searcherupdatepv)) records the full
principal variation, which the `info ... pv` lines print and whose second move is the
`bestmove ... ponder` reply ([`ponderMove`](#searcherpondermove), with a TT fallback).
Neither changes the searched tree: `bench` node counts are unchanged.

Each selective layer is behind a [`Tunables`](#struct-tunables) toggle so it can be
individually A/B-isolated — the discipline that caught step-1's history/IIR regressions.
Everything above still sits on the fail-soft **Principal Variation Search** negamax with
a [transposition table](tt.hpp.md).

## Source Files

- **Header (interface):** [search.hpp](search.hpp)
- **Source (implementation):** [search.cpp](search.cpp)

## Namespace

- Public API (`SearchState`, `Limits`, `Tunables`, `Searcher`) in namespace `engine`.
- `MOVE_OVERHEAD_MS`, `TIME_CHECK_MASK`, `ONLY_MOVE_MAX_DEPTH` (root depth cap for a
  timed search with a single legal move), `scoreToUci`, and the **structural**
  selective-search gates — `IIR_MIN_DEPTH`, `TT_CUTOFF_MAX_HALFMOVE`, `NMP_MIN_DEPTH` /
  `NMP_DIV` / `NMP_EVAL_MAX`, `RFP_MAX_DEPTH`, `FUT_MAX_DEPTH`, `LMP_MAX_DEPTH`,
  `LMR_MIN_DEPTH` / `LMR_MIN_MOVES`, `ASPIRATION_MIN_DEPTH`, `SINGULAR_TT_DEPTH_MARGIN`
  (how much shallower than the node the TT entry may be to seed a singular verification),
  `EXTENSION_PLY_FACTOR` (a node may extend only while `ply < EXTENSION_PLY_FACTOR ·
  rootDepth_`; see [the extension budget](#searchersearch)), `HIST_PRUNE_MAX_DEPTH` (depth gate for history pruning), `HIST_LMR_MAX_PLIES` (cap on
  the history-driven LMR adjustment), `RAZOR_MAX_DEPTH`, and the ProbCut trio
  `PROBCUT_REDUCTION` / `PROBCUT_TT_DEPTH_MARGIN` / `PROBCUT_MIN_SEE_THRESHOLD` (a
  `static_assert` pins `PROBCUT_TT_DEPTH_MARGIN > SINGULAR_TT_DEPTH_MARGIN`) —
  plus the helpers `lmpCount`
  and `hasNonPawnMaterial` live in an anonymous namespace in `search.cpp` (internal
  linkage). The **pruning margins** that used to be `constexpr` (`NMP_BASE`,
  `NMP_EVAL_DIV`, `RFP_MARGIN`, `FUT_MARGIN` / `FUT_BASE`, the LMR base/divisor, the LMP
  base) are now **runtime-tunable [`Tunables`](#struct-tunables) fields** so SPSA can
  perturb them; the LMR reduction table is a `Searcher` member rebuilt from them per
  search ([`buildReductions`](#searcherbuildreductions)). (The former `DELTA_MARGIN` /
  `ENDGAME_PIECES` q-search constants are likewise `Tunables` fields.)

## Objects / Interfaces

### `enum class SearchState`

The lifecycle of one search, held in a `std::atomic<SearchState>` that the
[uci](uci.hpp.md) thread owns and writes and the searcher only reads (polled every 2048
nodes by [`checkStop`](#searchercheckstop), and blocked on by
[`awaitRelease`](#searcherawaitrelease)). Every write is followed by `notify_all()` so a
waiting search wakes. All accesses use `std::memory_order_relaxed`: the value carries no
other data, and thread start/join supply the ordering around a search.

| Enumerator | Meaning |
|------------|---------|
| `Running` | A normal search; the clock (if any) runs. Also the resting value between searches. |
| `Pondering` | A `go ponder` search before `ponderhit`: the clock is stopped and `bestmove` is withheld. `ponderhit` moves it to `Running`. |
| `Stopped` | `stop` (or `quit`, a new `go`, `position`, `ucinewgame`): abort and report. |

**Used by:** [`Searcher`](#class-searcher), [uci](uci.hpp.md#class-engine), [bench](bench.hpp.md#run), [test_search.cpp.md](../tests/test_search.cpp.md), [test_qsearch.cpp.md](../tests/test_qsearch.cpp.md)

### `struct Limits`

Everything a UCI `go` command can specify, plus the soft node budget that
[datagen](datagen.hpp.md) uses. Populated by
[`Engine::handleGo`](uci.hpp.md#enginehandlego) (and by the datagen workers) and consumed
by [`Searcher::think`](#searcherthink).

| Field | Type | Description |
|-------|------|-------------|
| `depth` | `int` | Fixed depth cap in plies; `0` = no depth cap. |
| `movetime` | `int64_t` | Exact ms to spend on this move; `0` = none. |
| `nodes` | `int64_t` | **Hard** node cap: the search aborts mid-iteration once it is reached (the aborted iteration is discarded); `0` = none. |
| `softNodes` | `int64_t` | **Soft** node budget: no new iterative-deepening iteration is started once this many nodes have been spent, so the move always comes from a completed depth; `0` = none. Not settable through UCI `go`; [datagen](datagen.hpp.md#struct-datagenconfig) sets it (with `nodes` as a safety cap). Like `nodes`, it makes the search untimed. |
| `wtime` / `btime` | `int64_t` | Ms remaining on White's / Black's clock. |
| `winc` / `binc` | `int64_t` | Increment (ms) per move for White / Black. |
| `movestogo` | `int` | Moves until the next time control; `0` = sudden death (a default of 30 is assumed). |
| `infinite` | `bool` | Search until an explicit `stop`: `bestmove` is withheld until then, even after a forced mate is found. |
| `ponder` | `bool` | `go ponder`: the search starts in ponder mode (clock stopped, `bestmove` withheld until `ponderhit` or `stop`). The clock fields are the ones to use from the `ponderhit` on. |

**Used by:** [`Searcher::think`](#searcherthink), [bench](bench.hpp.md#run), [datagen](datagen.hpp.md#class-worker-internal)

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
| `ponderCreditPermille` | `int` | `500` | At `ponderhit`, this permille of the time already spent pondering is charged to the move's budget (`PonderCreditPermille`, 0–1000). `0` = the budget starts fresh at the hit (most depth on a correct guess); `1000` = pondering counts in full (often an instant move, banking clock). Untested default; meant for SPSA / SPRT. Applied by [`syncPonder`](#searchersyncponder). |
| `useStabilityTm` | `bool` | `true` | Stability time management (`UseStabilityTm`, 0/1): after each completed iteration from depth `TM_SCALE_MIN_DEPTH` (6), a clock search's soft limit is rescaled by the best root move's node share and how long it has held. See [`scaleSoftLimit`](#searcherscalesoftlimit-private). Off reproduces v0.3.1's fixed soft limit exactly. |
| `tmNodeBase` | `int` | `153` | Node-fraction term, ×100: `nodeScale = (tmNodeBase/100 − f) · tmNodeScale/100`, `f` = share of all nodes spent under the best root move (`TmNodeBase`, 100–300). |
| `tmNodeScale` | `int` | `174` | Node-fraction multiplier, ×100 (`TmNodeScale`, 50–400). With the defaults: `f = 0.5` → ×1.79, `f = 0.9` → ×1.10, `f = 1` → ×0.92. |
| `tmStability` | `std::array<int, 5>` | `{238, 129, 107, 91, 71}` | Best-move stability multipliers, ×100, indexed by the number of completed iterations the best move has held (capped at 4): a new best move gets ×2.38, one held 4+ iterations ×0.71 (`TmStab0`–`TmStab4`, 10–500). Defaults are Alexandria's SPSA-tuned values. |
| `tempo` | `Value` | `9` | Side-to-move bonus (cp) passed to [`evaluate`](eval.hpp.md#evaluate). |
| `deltaMargin` | `Value` | `203` | Q-search delta-pruning safety cushion (cp). |
| `endgamePieces` | `int` | `7` | Total piece count at/below which delta pruning switches off. |
| `useKillers` | `bool` | `true` | Enable the killer-move ordering signal (`UseKillers`). |
| `useHistory` | `bool` | `true` | Enable the butterfly-history ordering signal (`UseHistory`). |
| `useCountermove` | `bool` | `true` | Enable the countermove ordering signal (`UseCountermove`). |
| `useContHist` | `bool` | `true` | Enable continuation history (Phase 1c) (`UseContHist`). **Default on**: A/B-neutral at the material-only eval (−19 ± 24 Elo, 436 games) but +7 ± 8 Elo (51.0%, 3264 games) once the full HCE eval landed — its value scales with eval sophistication. |
| `useIir` | `bool` | `true` | Enable Internal Iterative Reduction (`UseIIR`). |
| `useNmp` | `bool` | `true` | Enable null-move pruning (`UseNMP`). |
| `useRfp` | `bool` | `true` | Enable reverse futility / static null-move pruning (`UseRFP`). |
| `useFutility` | `bool` | `true` | Enable futility pruning of quiets near the horizon (`UseFutility`). |
| `useLmp` | `bool` | `true` | Enable late-move (move-count) pruning (`UseLMP`). |
| `useLmr` | `bool` | `true` | Enable late move reductions (`UseLMR`). |
| `useCheckExt` | `bool` | `true` | Enable check extensions (`UseCheckExt`). |
| `useSingular` | `bool` | `true` | Enable singular extensions (Phase 1c) (`UseSingular`). |
| `useAspiration` | `bool` | `true` | Enable root aspiration windows (`UseAspiration`). |
| `useHistPrune` | `bool` | **`false`** | Enable history pruning of late quiets (Phase 1d) (`UseHistPrune`). Neutral at both margins tried (`2048` and `1024`), so it ships off (see the Phase 1d rationale below). |
| `useHistLmr` | `bool` | `true` | Enable the history-modulated LMR adjustment (Phase 1d) (`UseHistLmr`). **+17.7 ± 9.2 Elo** vs 0.1c. |
| `useCorrHist` | `bool` | `true` | Enable [correction history](corrhist.hpp.md) — the learned pawn-structure adjustment to the static eval (Phase 1d) (`UseCorrHist`). Off, the search sees the raw [`evaluate`](eval.hpp.md#evaluate) score. **+27.6 ± 11.3 Elo** vs 0.1c. |
| `useCaptureHist` | `bool` | **`false`** | Enable [capture history](history.hpp.md#historycapturescore) in the capture order (Phase 1d) (`UseCapHist`). Off, captures are ordered by SEE + MVV-LVA alone. **+2.3 ± 5.7 Elo** — neutral, so it ships off (see the Phase 1d rationale below). |
| `useRazoring` | `bool` | **`false`** | Enable razoring (Phase 1d) (`UseRazoring`). Ships **off** — PLAN.md §15 rates it a tune-last extra, so it is wired and inert until an SPRT says otherwise (which also keeps the bench signature stable). |
| `useProbCut` | `bool` | **`false`** | Enable ProbCut (Phase 1d) (`UseProbCut`). Ships **off**, same reasoning. |
| `aspirationDelta` | `Value` | `15` | Initial half-window (cp) around the previous score (`AspirationDelta`). |
| `lmrBase` | `int` | `63` | LMR reduction offset ×100 (`LmrBase`); `0.63` in the curve. |
| `lmrDivisor` | `int` | `142` | LMR `ln·ln` divisor ×100 (`LmrDivisor`); `1.42` in the curve (kept ≥ 1). |
| `nmpBase` | `int` | `1` | Null-move base reduction `R` (`NmpBase`). |
| `nmpEvalDiv` | `int` | `168` | Null-move eval-margin divisor (`NmpEvalDiv`) — cp of `eval − beta` per extra ply of `R`. |
| `rfpMargin` | `Value` | `68` | Reverse-futility margin per remaining ply, cp (`RfpMargin`). |
| `futMargin` | `Value` | `79` | Futility margin per remaining ply, cp (`FutMargin`). |
| `futBase` | `Value` | `73` | Futility base margin, cp (`FutBase`). |
| `lmpBase` | `int` | `4` | Late-move-pruning quiet-count base (`LmpBase`); count `= base + depth²`. |
| `singularMinDepth` | `int` | `7` | Min depth to attempt a singular extension; the TT entry must also be this deep (`SingularMinDepth`). |
| `singularMargin` | `int` | `1` | Singular-beta offset per ply: `singularBeta = ttValue − singularMargin·depth` cp (`SingularMargin`). |
| `singularDoubleMargin` | `int` | `46` | Extend by 2 plies when the verification score falls this far under `singularBeta`, cp (`SingularDoubleMargin`). |
| `histPruneMargin` | `int` | `2048` | History-pruning threshold per remaining ply (`HistPruneMargin`): a late quiet scoring below `−histPruneMargin·depth` is skipped. History-score units, not cp. |
| `histLmrDiv` | `int` | `7512` | History score per one ply of LMR adjustment (`HistLmrDiv`); kept `>= 1`. |
| `razorBase` | `Value` | `180` | Razoring base margin, cp (`RazorBase`). |
| `razorMargin` | `Value` | `160` | Razoring margin per remaining ply, cp (`RazorMargin`); the gate is `eval + razorBase + razorMargin·depth <= alpha`. |
| `probCutMinDepth` | `int` | `5` | Minimum depth at which ProbCut is attempted (`ProbCutMinDepth`). |
| `probCutMargin` | `Value` | `180` | ProbCut searches against `beta + probCutMargin`, cp (`ProbCutMargin`). |

The **forward-pruning margins** (`lmrBase` … `lmpBase`) and the **singular knobs**
(`singularMinDepth` / `singularMargin` / `singularDoubleMargin`) are exposed as UCI options
(not `constexpr`) so **SPSA** self-play can tune them without a rebuild. They were first
SPSA-tuned at the end of Phase 1b on the material-only eval (**+82 ± 20 Elo**), then
**re-tuned on the full Phase 1c HCE eval** (OpenBench, 11 params incl. the singular knobs,
96k games @ STC 10+0.1) — the re-tuned set above beat the Phase 1b defaults by **+26.2 ± 10
Elo** (SPRT accepted @ LTC 40+0.4). The `NmpBase` / `LmpBase` tuning floor was relaxed from
1 to 0 for that re-tune; `LmpBase` then settled at `4` and `NmpBase` stayed at `1`.
After Phase 1d landed correction history + history-LMR, the eval-gated margins and the LMR
curve were **re-tuned a third time** (OpenBench, 7 params: `lmrBase`, `lmrDivisor`,
`nmpEvalDiv`, `rfpMargin`, `futMargin`, `futBase`, `histLmrDiv`; 45k games @ STC 10+0.1).
The result — more LMR, slightly tighter futility, `rfpMargin` unchanged — beat the previous
defaults by **+6.1 ± 4.3 Elo** (9476 games, SPRT `[0, 5]` accepted @ LTC 40+0.4).
Only the margins are tunable; the structural gates (min depths, `LMR_MIN_MOVES`, etc.) stay
`constexpr`. `lmrBase` / `lmrDivisor` are integer-scaled ×100 because the tuner works in
integers; they are divided when the LMR table is (re)built
([`buildReductions`](#searcherbuildreductions)).

The `use*` toggles exist to **A/B-isolate** each selective signal's Elo (flip one on/off,
run an SPRT). `useKillers` / `useHistory` / `useCountermove` / `useContHist` are applied
via [`History::setEnabled`](history.hpp.md#historysetenabled) (called at the top of
[`think`](#searcherthink)); `useIir` / `useNmp` / `useRfp` / `useFutility` / `useLmp` /
`useLmr` / `useCheckExt` / `useSingular` each gate their step inside
[`search`](#searchersearch); and `useAspiration` / `aspirationDelta` drive
[`aspirationSearch`](#searcheraspirationsearch). With every selective toggle off the
search reproduces the plain PVS + step-1-ordering engine.

The **Phase 1d knobs** (`useHistPrune` / `histPruneMargin`, `useHistLmr` / `histLmrDiv`)
are denominated in **history-score units**, not centipawns: the quiet score they read is
the butterfly + continuation sum, which lives in `±(1 + CONT_PLIES) · MAX_HISTORY`
(`±49152`). `histLmrDiv` is SPSA-tuned (`8192` seed → `7512`); `histPruneMargin` stays a
seed, since history pruning ships off; the depth gate `HIST_PRUNE_MAX_DEPTH` and
the `HIST_LMR_MAX_PLIES` cap stay `constexpr`.

**Default rationale (Phase 1d, A/B @ STC 10+0.1, UHO book).** Every Phase 1d feature was
measured **alone**, against a run with *all* Phase 1d toggles off — a configuration that
reproduces release **v0.1c node-for-node** (1,947,551 nodes over four positions at depth
12) — so each number below is Elo **vs the 0.1c release**, measured on one binary through
UCI options rather than two builds:

| Feature | Result | Verdict |
|---|---|---|
| `UseCorrHist` | **+27.6 ± 11.3** Elo (53.97%, 1953 games) | SPRT `[0, 5]` **accepted** — the biggest Phase 1d gainer |
| `UseHistLmr` | **+17.7 ± 9.2** Elo (52.54%, 2912 games) | SPRT `[0, 5]` **accepted** |
| `UseHistPrune` | **+1.5 ± 4.9** Elo (50.22%, 10000 games) at `histPruneMargin = 2048`; **−0.7 ± 4.9** (49.90%, 10000 games) at `1024`, measured on top of correction history + history-LMR | **neutral** at both margins — ships **off** (history-LMR already consumes the same score) |
| `UseCapHist` | **+2.3 ± 5.7** Elo (50.32%, 7399 games) | **neutral** — run cut short at 7399 of 10000 games with the interval straddling zero; ships **off** (wired, so it can be re-tested after NNUE) |

The **shipping stack** — correction history + history-LMR, with history pruning and capture
history off — measured **+32.5 ± 11.6 Elo** vs the all-off (== 0.1c) configuration (54.66%,
1576 games, SPRT `[0, 5]` accepted).

Because each was measured in isolation, the numbers are **not additive**: they overlap
(history-LMR and history pruning read the same score; correction history moves the eval
that futility and RFP gate on), so the stack total is settled by a cumulative test, not by
summing the column. Correction history in particular under-read here — `rfpMargin` /
`futMargin` / `futBase` / `nmpEvalDiv` had been SPSA-tuned at `87fa8a1` against the
**uncorrected** eval's error distribution — so they were jointly re-tuned on top of this stack
(see the margins note above: **+6.1 ± 4.3 Elo** @ LTC).

The **Phase 1c knobs** (`useContHist`, and the singular set `useSingular` /
`singularMinDepth` / `singularMargin` / `singularDoubleMargin`) are new this phase; the
singular margins are exposed as UCI options so SPSA can tune them, while
`SINGULAR_TT_DEPTH_MARGIN` and the `(depth − 1) / 2` verification depth stay `constexpr`.

**Default rationale (A/B vs release 0.1b @ STC 10+0.1, UHO book).** Isolated:
**singular extensions +24 Elo** (`UseSingular` alone, `UseContHist` off — a gainer, so
**default on**); **continuation history −19 ± 24 Elo** at the material-only eval, which
**flipped to +7 ± 8 Elo** (51.0%, 3264 games) once the full HCE eval landed, so it is now
**default on**. This is [`History`](history.hpp.md)'s butterfly/IIR pattern again — those
signals were negative in isolation pre-pruning (−23 / −36) and flipped to large wins
(+247 / +16) once the rest of the stack existed to consume them: an ordering refinement's
Elo depends on how much positional structure the eval exposes.

**Default rationale (A/B @ 8+0.08 vs the same binary):** all four are **on**. Killers
**+44 Elo** and countermove **~0**. Butterfly **history** and **IIR** both *inverted*
once the pruning stack existed to consume them: history **−23 → +247** and IIR
**−36 → +16** (95% CI `[+4, +28]`), because LMP/LMR prune/reduce late-ordered quiets, so
good ordering and a hash-move-first search are what stop them discarding a winning move
("ordering makes pruning safe", PLAN.md Part 4). All four remain wired and UCI-tunable
for per-signal A/B isolation.

**Used by:** [`Searcher`](#class-searcher) (constructor), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::qsearch`](#searcherqsearch), [`Searcher::think`](#searcherthink), [`Searcher::search`](#searchersearch), [`evaluate`](eval.hpp.md#evaluate), [uci](uci.hpp.md#enginehandlesetoption)

### `class Searcher`

One search worker: one instance per search. Constructed with a shared
[`std::atomic<SearchState>&`](#enum-class-searchstate) (so the [uci](uci.hpp.md) thread
can stop it mid-search or end its pondering), a
[`TranspositionTable&`](tt.hpp.md#class-transpositiontable) it shares with the owner
(so results persist across moves and, later, across threads), and an optional
[`Tunables`](#struct-tunables) (defaulted for callers like [bench](bench.hpp.md) that
just want the defaults), and an optional [`nnue::Network`](nnue.hpp.md#struct-network)
pointer: with a network the static eval is NNUE, without one (the default, and what
[bench](bench.hpp.md) and [datagen](datagen.hpp.md) pass) it is the hand-crafted
[`evaluate`](eval.hpp.md#evaluate). See [`Searcher::Searcher`](#searchersearcher).

| Field | Type | Description |
|-------|------|-------------|
| `state_` | [`std::atomic<SearchState>&`](#enum-class-searchstate) | Shared search state, owned and written by the UCI thread; read here to stop, to leave ponder mode, and to wait before `bestmove`. |
| `tt_` | `TranspositionTable&` | Shared transposition table (owned by the caller; outlives the searcher). |
| `tp_` | `Tunables` | Self-play-tunable knobs (time, eval tempo, q-search delta) for this search. |
| `net_` | `const `[`nnue::Network`](nnue.hpp.md#struct-network)`*` | The NNUE network, **not owned** (the [uci](uci.hpp.md#class-engine-internal) `Engine` owns it and keeps it alive for the search); `nullptr` selects the hand-crafted eval. |
| `accumulators_` | `std::unique_ptr<std::array<`[`nnue::Accumulator`](nnue.hpp.md#struct-accumulator)`, MAX_PLY + 1>>` | One accumulator per ply (index = ply, 2 KB each, ~258 KB total). Heap-allocated once in the constructor, and only when `net_` is set, so the searcher stays small enough for a thread's stack. Entry 0 is refreshed from the root in [`think`](#searcherthink); every make writes entry `ply + 1`. |
| `nodes_` | `uint64_t` | Nodes visited in the current search. |
| `history_` | [`History`](history.hpp.md) | Quiet-move ordering heuristics (killers, butterfly + continuation history, countermoves) for this search. |
| `corrHist_` | [`CorrectionHistory`](corrhist.hpp.md#class-correctionhistory) | Learned per-pawn-structure static-eval correction for this search (64 KB, zeroed on construction). |
| `staticEvals_` | `Value[MAX_PLY]` | Per-ply static eval, for RFP / futility / the `improving` trend. `VALUE_NONE` at in-check plies. |
| `reductions_` | `uint8_t[LMR_DIM][LMR_DIM]` | LMR reduction table (`LMR_DIM` = 64), rebuilt from the tunable base/divisor each search by [`buildReductions`](#searcherbuildreductions). |
| `contPiece_` / `contTo_` | `int[MAX_PLY]` | Per-ply search stack: the colour+type piece (0–11) and to-square (0–63) of the move played into the next ply (`-1` for none / a null move). Written just before each recursion; read as `[ply-1]` / `[ply-2]` to build a node's continuation context. |
| `pv_` | `std::array<std::array<Move, MAX_PLY>, MAX_PLY>` | Triangular PV table: row `ply` holds the best line found from that ply (64 KB). Filled by [`updatePv`](#searcherupdatepv). |
| `pvLength_` | `std::array<int, MAX_PLY>` | Length of each `pv_` row; reset to 0 on entry to every [`search`](#searchersearch) call. |
| `rootPv_` / `rootPvLength_` | `std::array<Move, MAX_PLY>` / `int` | The root line of the last **completed** iteration (copied by [`saveRootPv`](#searchersaverootpv)), so an aborted iteration cannot corrupt the reported PV or the ponder move. |
| `start_` | `std::chrono::steady_clock::time_point` | Search start timestamp (the `go`, not the `ponderhit`). |
| `softLimitMs_` | `int64_t` | Soft budget in ms — no new depth is opened past it (`INT64_MAX` if untimed). Compared against [`clockMs`](#searcherclockms). |
| `hardLimitMs_` | `int64_t` | Hard budget in ms — the search aborts past it (`INT64_MAX` if untimed). Compared against [`clockMs`](#searcherclockms). |
| `nodeLimit_` | `int64_t` | Node cap; `0` = none. |
| `ponderDiscountMs_` | `int64_t` | Ponder time **not** charged to the budget: set once at the ponderhit to `pondered * (1000 - ponderCreditPermille) / 1000`; `0` otherwise. |
| `iterationStartMs_` | `int64_t` | `elapsedMs()` when the current iteration began; lets [`budgetSpent`](#searcherbudgetspent) abort, right after a ponderhit, an iteration the clock would never have started. |
| `rootDepth_` | `int` | Depth of the in-progress iterative-deepening iteration, set by [`think`](#searcherthink) before each one. Bounds the [extension budget](#searchersearch) in `search`. |
| `useTime_` | `bool` | Whether a wall-clock budget applies. |
| `timeUp_` | `bool` | Set once the search must abort; unwinds the recursion. |
| `pondering_` | `bool` | Still in ponder mode (no `ponderhit` seen yet): every clock check is skipped. Starts as [`Limits::ponder`](#struct-limits); cleared by [`syncPonder`](#searchersyncponder). |
| `rootMoveNodes_` | `std::array<uint64_t, 64 * 64>` | Nodes searched under each root move (index `from · 64 + to`), summed over every iteration of this `think` (32 KB, zeroed per `think`). Feeds the node fraction of [`scaleSoftLimit`](#searcherscalesoftlimit-private). |
| `softBaseMs_` | `int64_t` | The clock's soft limit as [`setupTiming`](#searchersetuptiming) computed it, before any stability scaling; every rescale starts from it. |
| `scaleSoft_` | `bool` | `true` only when a clock (`wtime`/`btime`) set the limits — never for `movetime`, depth, nodes or soft-node searches, which stability scaling must not touch. |
| `bestStreak_` | `int` | Completed iterations in a row whose best move matched the previous one's (`0` after a change). |
| `rootBest_` | `Move` | Best move of the in-progress iteration. |
| `rootBestCompleted_` | `Move` | Best move of the last **completed** iteration (the one actually played). |
| `rootScore_` | `Value` | Score of the last **completed** iteration (side-to-move cp; `VALUE_ZERO` until one completes). Read through [`score`](#searcherscore). |

**Methods:** [`Searcher::Searcher`](#searchersearcher), [`Searcher::think`](#searcherthink), [`Searcher::nodes`](#searchernodes), [`Searcher::score`](#searcherscore) (public); [`Searcher::staticEval`](#searcherstaticeval), [`Searcher::makeMove` / `makeNullMove`](#searchermakemove--searchermakenullmove-private), [`Searcher::search`](#searchersearch), [`Searcher::aspirationSearch`](#searcheraspirationsearch), [`Searcher::qsearch`](#searcherqsearch), [`Searcher::correctedEval`](#searchercorrectedeval), [`Searcher::buildReductions`](#searcherbuildreductions), [`Searcher::lmrReduction`](#searcherbuildreductions), [`Searcher::setupTiming`](#searchersetuptiming), [`Searcher::scaleSoftLimit`](#searcherscalesoftlimit-private), [`Searcher::checkStop`](#searchercheckstop), [`Searcher::syncPonder`](#searchersyncponder), [`Searcher::budgetSpent`](#searcherbudgetspent), [`Searcher::awaitRelease`](#searcherawaitrelease), [`Searcher::elapsedMs`](#searcherelapsedms), [`Searcher::clockMs`](#searcherclockms), [`Searcher::updatePv`](#searcherupdatepv), [`Searcher::saveRootPv`](#searchersaverootpv), [`Searcher::reportIteration`](#searcherreportiteration), [`Searcher::ponderMove`](#searcherpondermove), [`Searcher::printBestMove`](#searcherprintbestmove) (private).

**Used by:** [uci](uci.hpp.md), [bench](bench.hpp.md), [datagen](datagen.hpp.md)

## Functions

### `Searcher::think`

Run the full search: configure limits, then iterative-deepen until the depth cap,
time, or a forced mate stops it. Optionally prints an `info` line per completed
iteration ([`reportIteration`](#searcherreportiteration), with the full PV) and a final
`bestmove <move> [ponder <reply>]` ([`printBestMove`](#searcherprintbestmove)). A ponder
or infinite search that ends early (a forced mate, the depth cap, a node cap) waits in
[`awaitRelease`](#searcherawaitrelease) before reporting, as UCI requires. When a clock
governs the move (`useTime_`) and the position has exactly **one legal move**, the depth
cap drops to `ONLY_MOVE_MAX_DEPTH` (4): the move is forced, so the engine replies almost
instantly instead of spending its soft budget, while still emitting a score and PV. A
`go ponder` on a forced position also caps at 4, then holds its `bestmove` in
`awaitRelease` and answers at once on `ponderhit`.
**When to use:** once per move, from the search thread.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `Board` | Position to search. Taken **by value** — `think` searches on its own copy, so the caller's board is untouched. |
| `limits` | `const Limits&` | Depth/time/node bounds for this move. |
| `printBest` | `bool` | Emit the final `bestmove` line (default `true`). |
| `printInfo` | `bool` | Emit per-iteration `info` lines (default `true`). |

**Returns:** `Move` — the best move found (`rootBestCompleted_`). Returns
`Move::NO_MOVE` and prints `bestmove 0000` when the position has no legal moves (after
the same [`awaitRelease`](#searcherawaitrelease) wait).

**Side Effects:** writes to `std::cout` (`info` / `bestmove`, including a
`hashfull` field from [`tt_.hashfull()`](tt.hpp.md#transpositiontablehashfull));
calls [`tt_.newSearch()`](tt.hpp.md#transpositiontablenewsearch) once to advance the
TT generation; with a network, refreshes `accumulators_[0]` from the root position
([`nnue::refresh`](nnue.hpp.md#refresh)); resets and updates all timing/node/root-move/PV/ponder members,
and sets `rootDepth_` before each iteration (the [extension budget](#searchersearch)). Reads
the shared [`state_`](#enum-class-searchstate) (never writes it); may block on it at the
end. With `printBest`, probes the TT once more for the ponder reply.

**Warnings:** the first legal move is stored as a fallback **before** searching, so
`think` never returns a null move in a non-terminal position even if aborted at
depth 1. An aborted iteration is discarded (the previous completed iteration's move
stands). Soft stop: once [`clockMs`](#searcherclockms) reaches
[`softLimitMs_`](#class-searcher) no new depth is started; the current depth may still
run until the hard limit. After each completed iteration `think` updates `bestStreak_`
and calls [`scaleSoftLimit`](#searcherscalesoftlimit-private) (before the soft check),
so under a clock the soft limit moves with the root's stability. A [`softNodes`](#struct-limits) budget works the same way in
nodes: the iteration that crosses it completes, then the loop stops (checked right after
the iteration is recorded, before the clock's soft check). While pondering, neither limit applies — the search keeps
deepening until the `ponderhit`; [`syncPonder`](#searchersyncponder) runs before the
soft check so a hit is seen at the iteration boundary too. The root
depth is clamped to `MAX_PLY - 1` even when `go depth N` requests a larger `N`, so the
iterative-deepening loop itself cannot run past the per-ply tables. The only-move cap
applies **only to timed searches** (`wtime`/`btime` or `movetime`); `go depth`,
`go nodes`, and `go infinite` search a forced position as normal, so `bench` node counts
and analysis are unaffected. A node's `ply` can
still exceed the root depth via **check extensions**; what keeps the
[`History`](history.hpp.md) killers and `staticEvals_` in bounds there is the
`ply >= MAX_PLY - 1` leaf guard in [`search`](#searchersearch) / [`qsearch`](#searcherqsearch)
plus the `ply + 1 < MAX_PLY - 1` gate on the extension.

### `Searcher::reportIteration` (private)

Print one `info depth … score … nodes … nps … hashfull … time … pv …` line for a
completed iteration. The `pv` field is the whole of `rootPv_` (the last completed
iteration's line), not just the root move. `time` and `nps` count from the `go`, so a
ponder search reports its pondering time too. Cold path: once per iteration.

### `Searcher::ponderMove` (private)

Pick the opponent reply to ponder on after `rootBestCompleted_`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `Board&` | The root position (in/out): the best move is made and unmade on it, so it is restored before returning. |

**Returns:** `Move` — `rootPv_[1]` when the saved PV has at least two moves and starts
with the best move; otherwise the TT move stored for the position after the best move,
if that entry exists **and** the move is legal there; otherwise `Move::NO_MOVE` (then no
`ponder` token is printed).

**Warnings:** the TT fallback covers a PV cut to one move (e.g. the root's last
improvement came from a fail-high whose child left no line). The legality check guards
against a key collision handing back another position's move. Cold path: once per
search, with one legal-move generation.

### `Searcher::printBestMove` (private)

Write `bestmove <move>` plus ` ponder <reply>` when [`ponderMove`](#searcherpondermove)
finds one, then flush. Called by [`think`](#searcherthink) when `printBest`.

### `Searcher::nodes`

Return `uint64_t` — nodes visited in the current/last search. Read by
[bench](bench.hpp.md#run) to accumulate the benchmark node total. Trivial getter.

### `Searcher::score`

Return [`Value`](types.hpp.md#using-value--int) — the score of the last **completed**
iteration of the last [`think`](#searcherthink), in centipawns from the side to move's
point of view (a mate score when a mate was found). `VALUE_ZERO` when no iteration
completed (no legal moves). [datagen](datagen.hpp.md#class-worker-internal) records it
as each position's training label. Trivial getter.

### `Searcher::search` (private)

The recursive fail-soft negamax, refined with **Principal Variation Search** and the
full Phase 1b selective-search stack (returns `best`). Order of operations:

1. draw check → **leaf / ply guard → [`qsearch`](#searcherqsearch)** (when `depth <= 0`
   or `ply >= MAX_PLY - 1`);
2. **TT probe** (cutoff only off the PV) → **static eval** (raw value reused from the TT
   when present, then [correction history](corrhist.hpp.md) applied on top; skipped in
   check) → **`improving`** trend;
3. **whole-node pruning** (off-PV, not in check, away from mate): **razoring** → **RFP** →
   **NMP** → **ProbCut**;
4. **IIR** (gated) → movegen → resolve the **continuation context** (the moves 1/2 plies
   back, from the search stack) → **[`orderMoves`](movepick.hpp.md#ordermoves)**;
5. **PV reset**: `pvLength_[ply] = 0` on entry (before any return), except inside a
   singular verification, which re-searches the *same* ply and must not wipe the line
   its parent node is building there;
6. **move loop**: per-move quiet pruning (**LMP**, **futility**, **history pruning**) → **singular extension**
   (TT move only, pre-make) → record the search-stack move → make → **check extension** →
   **LMR** reduction → **PVS** search/re-search → cutoff bookkeeping (quiet-cutoff
   [`history`](history.hpp.md#historyupdatequietcutoff) updates, including continuation
   history) and, at a PV node, an [`updatePv`](#searcherupdatepv) whenever a move
   raises alpha;
7. **TT store** (value, best move, bound, and the static eval) — **skipped** during a
   singular-verification search (`excluded` set).

`pvNode` is derived as `beta - alpha > 1`: the root and full-window nodes are PV; the
null-window scout nodes are not. Every forward-pruning / reduction step is restricted to
non-PV nodes (and skipped in check and near mate scores) so the principal variation is
searched exactly.

**Selective-search steps** (each gated by its [`Tunables`](#struct-tunables) toggle):

- **Static eval & `improving`** — at a non-check node the **raw** static eval is read from
  the TT (`tt.eval`) or computed by [`staticEval`](#searcherstaticeval) (NNUE or HCE), then
  [`correctedEval`](#searchercorrectedeval) adds the
  [correction-history](corrhist.hpp.md) adjustment for this pawn structure; the
  **corrected** value is what lands in `staticEvals_[ply]` and drives every downstream
  test. `improving` is true when this eval exceeds the same side's eval
  two plies ago (`staticEvals_[ply-2]`) — pruning is a touch more aggressive when the
  position is *not* improving.
- **Correction history** — after the move loop, a node whose verdict is trustworthy
  teaches [`corrHist_`](corrhist.hpp.md#correctionhistoryupdate) the residual
  `best − eval`: not in check, a real **quiet** best move, no mate score, not inside a
  singular verification, and the bound must agree with the residual's sign (`BOUND_LOWER`
  may only teach "the eval was too low", `BOUND_UPPER` only "too high"). The **raw** eval —
  never the corrected one — is what goes into the TT, so a revisit corrects once rather
  than compounding.
- **Razoring** (default **off**) — near the horizon (`depth <= RAZOR_MAX_DEPTH`), a static
  eval so far under alpha that `eval + razorBase + razorMargin·depth <= alpha` is verified
  by a null-window [`qsearch`](#searcherqsearch) instead of a full-width search; if the
  q-search also fails low, its score is returned. Two guards beyond the usual ones: it is
  an **alpha-side** prune returning a q-search score, and this q-search reports a stalemate
  as its stand-pat eval rather than `VALUE_DRAW`, so razoring takes the same
  `hasNonPawnMaterial` guard as NMP; and it never runs inside a singular verification,
  where a cheap fail-low would fake a singular move.
- **ProbCut** (default **off**) — at `depth >= probCutMinDepth`, a capture that beats a
  raised beta (`probBeta = beta + probCutMargin`) is treated as evidence the node fails
  high. Only captures whose [`seeGE`](see.hpp.md#seege) clears `probBeta − eval` (floored at
  `PROBCUT_MIN_SEE_THRESHOLD`) are tried; each is screened by a null-window q-search and,
  if that clears `probBeta`, verified by a real search at `depth − PROBCUT_REDUCTION`. A
  success stores a `BOUND_LOWER` entry at `depth − PROBCUT_TT_DEPTH_MARGIN` and returns.
  Storing that bound **shallower than `SINGULAR_TT_DEPTH_MARGIN`** is load-bearing: at
  `depth − 3` a deliberately optimistic ProbCut bound would qualify as a
  singular-verification target and manufacture spurious extensions. Skipped when the TT
  already refutes `probBeta` at that depth, and never inside a singular verification.
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
- **History pruning** — near the horizon (`depth <= HIST_PRUNE_MAX_DEPTH`) a quiet whose
  combined [`quietHistory`](history.hpp.md#historyquiethistory) score falls below
  `−histPruneMargin · depth` is skipped: it has been refuted everywhere it was tried, so
  it is not worth a node. The score is read **once per move** and shared with the LMR
  adjustment below, so the extra cost is one butterfly + up-to-two continuation lookups
  per quiet (the same entries ordering just touched).
- **Singular extension** — for the **TT move only**, decided *before* it is made. When
  the TT entry is a deep enough lower bound (`tt.depth >= depth − SINGULAR_TT_DEPTH_MARGIN`,
  bound `LOWER`/`EXACT`, `depth >= singularMinDepth`, not a mate score, not already inside
  a verification search), a reduced-depth search at `(depth − 1) / 2` that **excludes** the
  TT move is run against a lowered beta `singularBeta = ttValue − singularMargin·depth`. If
  every alternative fails below `singularBeta` the TT move is *singular* and extended by 1
  ply (by **2** off the PV when they fail more than `singularDoubleMargin` under); if it is
  not singular yet the TT score already beats beta, it is reduced by 1 (a multi-cut-style
  negative extension). The exclusion is threaded via the `excluded` parameter (below).
- **Check extension** — a move that gives check is searched one ply deeper, **unless** it
  already earned a positive singular extension (the two never stack), and bounded by
  `ply + 1 < MAX_PLY - 1` so `ply` can never overrun the per-ply tables:
  `newDepth = depth − 1 + singularExt + (extendCheck ? 1 : 0)`.
- **Extension budget** — both positive extensions (singular, check) are only granted while
  `ply < EXTENSION_PLY_FACTOR · rootDepth_` (2× the iteration's depth), the
  Stockfish-style cap; a node past it does not even run the singular verification. The
  negative singular extension is skipped there too. Without the cap, and with singular and
  check extensions stacking, a checking line that kept being singular *gained* depth every
  ply: in a queen endgame (KQvK) an iteration exploded — v0.2 completed depth 10 in 0.02 s
  and then never finished depth 11 in 3 s, ran into the hard time limit nearly every move,
  and drew 8/8 KQvK test games by the fifty-move rule. With the cap and no stacking the same
  position reaches depth 19 and 7/8 of those games are mated (6–16 moves). `bench` is
  unchanged (its depth never reaches the cap). Strength vs v0.2 @ STC 10+0.1, UHO book:
  **+8.5 ± 3.8 Elo** (51.22%, 12212 games, SPRT `[-1.75, 0.25]` H1 accepted) — a fix that
  also gains, not just a non-regression.
- **Late move reductions (LMR)** — late quiet, non-checking moves are searched at
  `newDepth - reduction`, where `reduction` comes from the `ln(depth)·ln(moveCount)`
  table, reduced by one on the PV and one when `improving`, then (when `useHistLmr`)
  adjusted by the move's own history: `reduction -= clamp(histScore / histLmrDiv,
  −HIST_LMR_MAX_PLIES, +HIST_LMR_MAX_PLIES)`, so a quiet with good history is reduced
  less and one with bad history more. Finally clamped to keep the reduced depth `>= 1`. A
  reduced search that beats alpha is re-searched at full `newDepth` (null window), then at
  the full window if it lands in `(alpha, beta)`.

The per-move quiet pruning (futility/LMP/history) and reductions never touch the first move,
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

**Continuation history:** the node reads its **search stack** (`contPiece_` / `contTo_`)
to build a [`ContHistContext`](history.hpp.md#struct-conthistcontext) — the `(piece, to)`
of the moves 1 and 2 plies back — which is passed to `orderMoves` (so a quiet's ordering
score folds in [`continuationScore`](history.hpp.md#historycontinuationscore)) and to
`updateQuietCutoff`. Each move records `contPiece_[ply]` / `contTo_[ply]` just before
recursing (a null move records `-1`), so a child reads `[ply-1]` and a grandchild
`[ply-2]`. The context is empty at the root, after a null move, or before enough plies
exist.

**Capture-cutoff learning:** on a fail-high caused by a **capture**,
[`history_.updateCaptureCutoff`](history.hpp.md#historyupdatecapturecutoff) rewards it and
penalises the captures tried before it (tracked in a per-node `capturesTried` stack array,
the capture twin of `quietsTried`). Killers, countermoves and the quiet tables stay
untouched.

**Quiet-cutoff learning:** on a fail-high (`alpha >= beta`) caused by a **quiet** move,
[`history_.updateQuietCutoff`](history.hpp.md#historyupdatequietcutoff) rewards that move,
penalises the quiet moves tried before it (tracked in a per-node `quietsTried` stack
array), and updates killers, the countermove for `prevMove`, and the continuation history
for the node's context. Capture cutoffs do not touch history. These signals feed the next
node's `orderMoves`.

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
| `excluded` | `Move` | Move to skip in this node's move loop (defaults to `NO_MOVE`). Set only by a **singular-verification** call — the same node/position searched with the TT move excluded; while set, the TT cutoff and the TT store are both suppressed and singular extensions are not re-attempted. |

**Returns:** [`Value`](types.hpp.md#using-value--int) — the negamax score of
`board`. `mated_in(ply)` at checkmate, `VALUE_DRAW` at stalemate/draw, `VALUE_ZERO`
when aborting (`timeUp_`). A singular-verification call (with `excluded` set) returns the
best score among the *alternatives* to the TT move (`-VALUE_INFINITE` if the TT move is the
only legal move, which the caller reads as "singular").

**Transposition table use:**
- **Probe** at [`board.hash()`](../include/chess.hpp). A hit yields the stored move
  (used for ordering) and its cached static eval, and — **at a non-PV node** (and **not**
  inside a singular-verification search) with `entry.depth >= depth` and a low fifty-move
  clock (`halfMoveClock() < TT_CUTOFF_MAX_HALFMOVE`, 90) — an immediate cutoff if the bound
  qualifies: `EXACT` always, `LOWER` when `ttv >= beta`,
  `UPPER` when `ttv <= alpha`, where `ttv = valueFromTT(entry.value, ply)`. The
  `!pvNode` guard means the root (and every PV node) **never cuts**, so a move is always
  produced and the PV stays exact. The verification search must **not** cut here (that
  would just return the excluded move's own score), hence the extra guard.
- **Ordering**: the probed TT move is passed to
  [`orderMoves`](movepick.hpp.md#ordermoves) together with [`history_`](history.hpp.md),
  the `ply`, and `prevMove`, which floats the TT move first and then sorts the rest
  (good captures by SEE+MVV-LVA, killers, countermove, history-scored quiets, losing
  captures).
- **Store** after the loop (unless aborted, **and unless `excluded` is set**): `depth`,
  `valueToTT(best, ply)`, the node's **static eval** (`VALUE_NONE` when in check), the best
  move, and the bound — `LOWER` if `best >= beta`, `EXACT` if `best > alphaOrig`, else
  `UPPER`. `alphaOrig` is captured before the loop mutates `alpha`. The stored eval is what
  a later probe reuses instead of recomputing [`evaluate`](eval.hpp.md#evaluate). A
  singular-verification result is the value of a **restricted** move set, so it is not
  stored under this position's key.
- **Prefetch**: `tt_.prefetch(board.zobristAfter(m))` before each `makeMove`.

**Side Effects:** mutates `board` transiently; probes/stores `tt_`; updates `nodes_`,
`history_` (on quiet cutoffs), the PV table (`pv_` / `pvLength_` at this ply), and, at
`ply == 0`, `rootBest_`; periodically calls `checkStop`.

**Warnings:** draw detection is skipped at the root (`ply > 0` guard) so a move is
always returned. Uses `isRepetition(1)` (twofold) as the in-search draw rule — the
library's default is threefold. On abort, partial results are discarded and **not**
stored in the TT. A stale/illegal TT move from a key collision is harmless: it is
only searched if it matches a generated legal move (the swap is a no-op otherwise).
Mate scores are rebased through the TT via [`valueToTT`/`valueFromTT`](tt.hpp.md#valuetott--valuefromtt).

### `Searcher::updatePv` (private)

Triangular PV update, called when move `m` raises alpha at a PV node: row `ply` becomes
`m` followed by row `ply + 1` (the child's line), and `pvLength_[ply]` becomes the
child's length + 1. The child row is always fresh — every `search` call resets its own
length on entry — so a child that found no line (a leaf, a draw, a fail-low) contributes
nothing. Only reached below the `ply >= MAX_PLY - 1` leaf guard, so `ply + 1` and the
copied length stay inside the table.

**Performance:** runs only on alpha improvements at PV nodes (a small fraction of
nodes); the per-node cost of the PV is the single length reset in
[`search`](#searchersearch). It never influences move choice, so `bench` is unchanged.

### `Searcher::saveRootPv` (private)

Copy row 0 of the PV table into `rootPv_` / `rootPvLength_` when an iteration completes,
so the next (possibly aborted) iteration cannot overwrite the reported line.

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

### `Searcher::Searcher`

Construct a searcher. Stores the shared `state` and `tt` references, copies `tp`, and
keeps `net` (not owned). With a network it also allocates `accumulators_` (one heap
allocation per searcher, never per node).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `state` | `std::atomic<`[`SearchState`](#enum-class-searchstate)`>&` | Shared lifecycle flag the search polls. |
| `tt` | [`TranspositionTable&`](tt.hpp.md#class-transpositiontable) | Shared TT; must outlive the searcher. |
| `tp` | [`Tunables`](#struct-tunables) | Knobs for this search (default `{}`). |
| `net` | `const `[`nnue::Network`](nnue.hpp.md#struct-network)`*` | NNUE network (default `nullptr` = hand-crafted eval); must outlive the searcher. |

### `Searcher::staticEval` (private)

Return the raw static eval ([`Value`](types.hpp.md#using-value--int), side-to-move cp) of
the node at `ply`: [`nnue::evaluate`](nnue.hpp.md#evaluate) on `accumulators_[ply]` and `board` (whose piece count picks the output bucket) when a
network is loaded, else [`evaluate`](eval.hpp.md#evaluate)`(board, tp_.tempo)`. The
network ignores `Tempo` (it learned the side-to-move effect itself). The result is what
goes into the TT; [`correctedEval`](#searchercorrectedeval) is applied on top as before.
**Performance:** per node; the NNUE path is one 2048-wide SCReLU dot product against the bucket's output row.

### `Searcher::makeMove` / `Searcher::makeNullMove` (private)

Make `move` (or a null move) on `board` at `ply`, first writing the child's accumulator
`accumulators_[ply + 1]`: [`nnue::update`](nnue.hpp.md#update) from the parent for a real
move, a plain copy for a null move (no piece moves). Without a network they reduce to
`board.makeMove` / `board.makeNullMove`. Every move made inside the tree goes through them
(main search, NMP, ProbCut, q-search); unmake needs no counterpart, since the parent's
accumulator at `ply` is never overwritten by its children. A singular verification
re-searches the same ply, so it reuses that ply's accumulator unchanged.
**Performance:** per make; a quiet move is one fused add+subtract pass over 512 int16s per
perspective, a capture or castle one more column.

### `Searcher::correctedEval` (private)

The static eval as the search reasons about it: `raw` plus the
[correction-history](corrhist.hpp.md#correctionhistorycorrected) adjustment for this
position's pawn structure and side to move, or `raw` unchanged when
[`tp_.useCorrHist`](#struct-tunables) is off.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position being evaluated (keys the correction bucket). |
| `raw` | `Value` | The raw [`evaluate`](eval.hpp.md#evaluate) score — passed in so the caller can reuse a TT-cached static eval instead of re-evaluating. |

**Returns:** [`Value`](types.hpp.md#using-value--int) — side-to-move-relative cp, clamped
clear of the mate range.

**Side Effects:** none (reads `corrHist_`).

**Performance:** per node — one pawn-bitboard mix plus one array read on top of the eval.

**Warnings:** the corrected value is what feeds RFP / futility / NMP / `improving` and
the q-search stand-pat, but the **raw** value is what is written to the TT
([`Searcher::search`](#searchersearch)); storing the corrected one would re-correct on
every revisit.

### `Searcher::buildReductions` (private)

(Re)build the `reductions_` LMR table from the tunable curve
`r(depth, moveCount) ≈ lmrBase/100 + ln(depth)·ln(moveCount)/(lmrDivisor/100)`. Called
once per search from [`think`](#searcherthink) (the base/divisor are UCI-tunable, so the
table cannot be a compile-time constant). Row/column 0 stay 0 and are never indexed;
`lmrDivisor` is floored at 1 to avoid a divide-by-zero. [`lmrReduction`](#searcherbuildreductions)
reads the table with the depth/move-count saturated at `LMR_DIM - 1`, where reductions
plateau. Rebuilding a 64×64 table once per `go` is negligible against a full search.

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
- **Stand-pat**: when *not* in check, [`staticEval`](#searcherstaticeval)`(board, ply)`
  — passed through [`correctedEval`](#searchercorrectedeval), so q-search and the main
  search agree on what a position is worth —
  seeds `best` as a lower bound; return immediately on `best >= beta`, else raise `alpha`. When
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
- **depth / nodes / softNodes / infinite**: no clock (`useTime_` stays false; runs untimed).
- **clock (`wtime`/`btime`…)**: `base = remaining/movestogo + inc/2` (movestogo
  defaults to `tp_.assumedMovestogo`), then soft/hard scale off `base` by permille.
  The hard limit is capped at half the remaining clock (minus overhead) so the
  engine never flags, and soft is clamped to ≤ hard. This branch alone also stores the
  soft limit as `softBaseMs_` and sets `scaleSoft_`, enabling
  [`scaleSoftLimit`](#searcherscalesoftlimit-private).

### `Searcher::scaleSoftLimit` (private)

Stability time management. Called by [`think`](#searcherthink) after every completed
iteration with its `depth`; a no-op unless `scaleSoft_` (a clock search),
`tp_.useStabilityTm`, and `depth ≥ TM_SCALE_MIN_DEPTH` (6; shallower iterations are too
cheap and noisy to judge).

```
f          = rootMoveNodes_[best] / nodes_          (share of all nodes under the best move)
nodeScale  = (tmNodeBase/100 − f) · tmNodeScale/100
stabScale  = tmStability[min(bestStreak_, 4)] / 100
softLimitMs_ = clamp(softBaseMs_ · nodeScale · stabScale, 1, hardLimitMs_)
```

A contested root (the best move took a small share of the effort, or just changed)
gets more time; a settled one (large share, held for several iterations) stops early.
Each call scales the **base** soft limit, so factors never compound across iterations,
and the result never passes the hard limit. While pondering it still runs, so the limit
[`budgetSpent`](#searcherbudgetspent) and the soft check see after a `ponderhit` is
already scaled. Untimed searches (bench, datagen's soft nodes, `movetime`) are untouched,
so the bench signature does not change.

**Root node accounting** (in [`search`](#searchersearch)): at `ply == 0`, outside a
singular verification (which would count the same moves twice), the nodes searched under
each root move are added to `rootMoveNodes_[from · 64 + to]` right after its unmake —
aborted moves included, since only the ratio matters.

### `Searcher::checkStop` (private)

Return `true` and set `timeUp_` if the search must stop: the shared
[`state_`](#enum-class-searchstate) is `Stopped`, the node cap is reached, or
[`budgetSpent`](#searcherbudgetspent) says the clock budget is used up. First calls
[`syncPonder`](#searchersyncponder) so a `ponderhit` is noticed within 2048 nodes.
Called every 2048 nodes (`TIME_CHECK_MASK`) from [`search`](#searchersearch) and
[`qsearch`](#searcherqsearch).

### `Searcher::syncPonder` (private)

Leave ponder mode once the UCI thread has moved [`state_`](#enum-class-searchstate) from
`Pondering` to `Running`: clears `pondering_` and sets `ponderDiscountMs_ = elapsed *
(1000 - ponderCreditPermille) / 1000`, so from here on [`clockMs`](#searcherclockms)
charges only `ponderCreditPermille` of the time already pondered. A no-op when not
pondering or before the hit. Called from [`checkStop`](#searchercheckstop) and once per
completed iteration in [`think`](#searcherthink); the hit is therefore applied at most
2048 nodes (well under a millisecond) late.

### `Searcher::budgetSpent` (private)

Return `bool` — whether this move's clock budget is used up. Always `false` without a
clock (`useTime_` off) or while pondering. Otherwise `true` when
[`clockMs`](#searcherclockms) has reached the hard limit, **or** when the in-progress
iteration began (in budget time, `iterationStartMs_ - ponderDiscountMs_`) at or past
the soft limit. Outside pondering an iteration always begins before the soft limit (the
soft check ends the loop first), so the second clause only fires right after a
`ponderhit`: it drops a deep iteration the clock would never have started and plays the
last completed one at once. Since the budget always charges at least the real time since
the `ponderhit`, the hard limit's never-flag cap still holds in wall-clock terms.

### `Searcher::awaitRelease` (private)

Block until the GUI releases the search: while [`state_`](#enum-class-searchstate) is
`Pondering`, or — when `infinite` — `Running`. Uses C++20 `std::atomic::wait`, woken by
the UCI thread's `notify_all()` on every state change, so it burns no CPU. Returns
immediately for an ordinary timed/depth/node search.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `infinite` | `bool` | [`Limits::infinite`](#struct-limits): also wait out `Running` until `stop`. |

### `Searcher::elapsedMs` (private)

Return `int64_t` — milliseconds since `start_` (the `go`). Used for `info` timing. Trivial.

### `Searcher::clockMs` (private)

Return `int64_t` — the time charged to this move's budget: `elapsedMs() -
ponderDiscountMs_`. Equal to `elapsedMs()` for a search that never pondered. All soft /
hard limit checks use it.
