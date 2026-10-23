# Module: `datagen`

Self-play **training-data generation** for the Phase 2 NNUE ([PLAN.md](../PLAN.md) §20,
"Datagen design"). The `chessai datagen` subcommand runs one independent **worker per
thread**. Each worker plays HCE self-play games from random openings, searches every
position with a node-limited [`Searcher`](search.hpp.md#class-searcher), and appends the
games to its own [viriformat](viriformat.hpp.md) file, which `bullet`'s
`ViriBinpackLoader` trains on directly. The design choices (format, budget, openings,
adjudication) were picked against alternatives in PLAN.md §20.

This is a cold driver around the search: it allocates (one buffer per worker, a board copy
per move) and does file I/O, but never inside the per-node search path. Workers share
nothing but a set of atomic progress counters, so they scale with cores without locks.

## Source Files

- **Header (interface):** [datagen.hpp](datagen.hpp)
- **Source (implementation):** [datagen.cpp](datagen.cpp)

## Related Test Documentation

- [test_datagen.cpp.md](../tests/test_datagen.cpp.md)

## How to Build & Run

Built into the engine (see [build.md](../build.md)); dispatched by [main](main.cpp.md):

```
./chessai datagen <out-prefix> <threads> <positions> [seed] [soft-nodes]
./chessai datagen /mnt/c/Users/sivuc/nnue-data/run1 22 300000000
```

Worker *i* **appends** to `<out-prefix>-<i>.vf`. A start line, then a progress line every
10 s and a final one, go to stdout:

```
datagen: 22 threads, target 300000000 positions, 5000 soft nodes, seed 1234, output run1-*.vf
datagen: 421 games, 53166 positions, 5308 pos/s, 10 s
```

Measured on the Ryzen 9 5900X (22 threads, 5k soft nodes): about **5.4k positions/s**,
i.e. roughly 15 h per 300M positions. Train on the files with bullet's
`ViriBinpackLoader` (`Filter::default()` drops the opening, in-check, and tactical
positions and mate scores).

## Namespace

- Public API (`DatagenConfig`, `Adjudicator`, `runDatagen`) in namespace `engine`.
- Internal linkage (anonymous namespace in `datagen.cpp`): the constants `HASH_MB`,
  `HARD_NODE_FACTOR`, `OPENING_PLIES`, `OPENING_EXTRA_PLIES`, `OPENING_MAX_SCORE`,
  `SEED_STRIDE`, `PROGRESS_POLL`, `PROGRESS_INTERVAL`; the types `Progress`,
  `SearchResult`, and [`Worker`](#class-worker-internal); and the helpers `outputPath`,
  `gamePly`, `outcomeOf`, `printProgress`, `reportUntilDone`.

## Objects / Interfaces

### `struct DatagenConfig`

One generator run, built from the command line by [main](main.cpp.md#main).

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `outPrefix` | `std::string` | — | Output path prefix; worker *i* appends to `"<outPrefix>-<i>.vf"`. |
| `threads` | `int` | `1` | Number of independent workers (one `std::jthread` each); `>= 1`. |
| `positions` | `uint64_t` | `0` | Stop once this many positions (plies recorded) have been written in total; `> 0`. |
| `seed` | `uint64_t` | `0` | Base RNG seed. Worker *i* seeds its `std::mt19937_64` with `seed ^ (SEED_STRIDE · (i + 1))`. [main](main.cpp.md#main) picks a random seed when none is given and the start line prints it. |
| `softNodes` | `int64_t` | `5000` | Per-move soft node budget ([`Limits::softNodes`](search.hpp.md#struct-limits)); the hard cap is `softNodes × HARD_NODE_FACTOR` (20). |

**Used by:** [`runDatagen`](#rundatagen), [main](main.cpp.md)

### `class Adjudicator`

Ends decided and dead games early from the **white-relative** search score, so compute
isn't spent on lopsided or drawn endgames. One instance per game, fed every ply.

| Constant | Value | Meaning |
|----------|-------|---------|
| `WIN_SCORE` | `2500` | `\|score\|` (cp) at or past which a position counts as decided. |
| `WIN_PLIES` | `4` | Consecutive decided plies, in the same side's favour, that call a win. |
| `DRAW_MIN_PLY` | `80` | No draw is called before this game ply (counted from the standard start). |
| `DRAW_SCORE` | `10` | `\|score\|` (cp) at or under which a position counts as dead level. |
| `DRAW_PLIES` | `10` | Consecutive level plies (all at `>= DRAW_MIN_PLY`) that call a draw. |

| Field | Type | Description |
|-------|------|-------------|
| `whiteWinPlies_` | `int` | Current streak of plies with `score >= WIN_SCORE`. |
| `blackWinPlies_` | `int` | Current streak of plies with `score <= -WIN_SCORE`. |
| `drawPlies_` | `int` | Current streak of late, level plies. |

Each streak resets to zero on any ply that breaks it. A found mate (`±(32000 − ply)`) is
far past `WIN_SCORE`, so a mating line is adjudicated after `WIN_PLIES` plies rather than
played out.

**Methods:** [`Adjudicator::update`](#adjudicatorupdate). **Used by:** `Worker::playGame`.

### `class Worker` (internal)

One generator thread's private state, heap-allocated (it embeds a multi-megabyte
`Searcher`) and never shared:

| Field | Type | Description |
|-------|------|-------------|
| `progress_` | `Progress&` | Shared counters: `positions`, `games` (relaxed atomics), and `running` (workers still active; decremented with release order when the worker exits). Owned by `runDatagen`, which outlives every worker. |
| `out_` | `std::ofstream` | `<outPrefix>-<i>.vf`, opened in binary **append** mode, so a re-run adds games and never truncates data. |
| `rng_` | `std::mt19937_64` | Drives the random openings. |
| `state_` | `std::atomic<SearchState>` | The [`SearchState`](search.hpp.md#enum-class-searchstate) its searches poll; always `Running` (node limits end each search). |
| `tt_` | [`TranspositionTable`](tt.hpp.md#class-transpositiontable) | `HASH_MB` (16) MB, cleared at the start of every game so games are independent. |
| `searcher_` | `std::optional<Searcher>` | Re-emplaced for each move (see below). |
| `limits_` | [`Limits`](search.hpp.md#struct-limits) | `softNodes = config.softNodes`, `nodes = softNodes × HARD_NODE_FACTOR`. |
| `game_` | [`viriformat::Game`](viriformat.hpp.md#class-game) | The game being recorded, reused across games. |

**The game loop** (`Worker::run`), repeated until the shared position target is reached:

1. **Opening** (`randomOpening`): from startpos, play `OPENING_PLIES` (8) uniformly random
   legal moves, plus one more with probability ½ so either side can move first. Restart if
   the game ends on the way. Search the result with the normal budget and **reject** it if
   `|score| > OPENING_MAX_SCORE` (1000 cp): a start the engine already calls decided would
   only teach one-sided positions.
2. **Play** (`playGame`): at each ply, stop if `board.isGameOver()` reports mate or a draw
   by rule (threefold repetition, fifty moves, insufficient material, stalemate). Otherwise
   search, record `(move, white-relative score)`, feed the
   [`Adjudicator`](#class-adjudicator), and stop if it calls the game; else make the move.
3. **Write** the game with its [`Outcome`](viriformat.hpp.md#enum-class-outcome), flush, and
   add its positions to the shared counter.

**One Searcher per move.** `searchPosition` emplaces a fresh
[`Searcher`](search.hpp.md#class-searcher) (cleared history and correction history) for
every move, exactly as the UCI `go` handler does, so the data reflects the engine as it
plays; only the TT carries over between the moves of one game. `std::optional::emplace`
rebuilds it in place, without an allocation per move. It runs
[`think`](search.hpp.md#searcherthink) silently (`printBest=false, printInfo=false`) and
reads [`score`](search.hpp.md#searcherscore).

**Determinism.** A worker's games depend only on its seed: openings come from its RNG and
searches are node-limited (no clock). Two runs with the same seed and thread count
reproduce the same games, which is why [main](main.cpp.md#main) picks a **random** seed by
default: appending a second run with the same seed would duplicate every game.

## Functions

### `Adjudicator::update`

Feed one ply's score and return whether the game can be called.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `whiteScore` | `Value` | Search score of the position, centipawns from **White's** view. |
| `ply` | `int` | Game ply of that position, counted from the standard start position (so the opening plies count). |

**Returns:** `std::optional<`[`viriformat::Outcome`](viriformat.hpp.md#enum-class-outcome)`>` —
`WhiteWin` / `BlackWin` once that side's winning streak reaches `WIN_PLIES`, `Draw` once the
level streak reaches `DRAW_PLIES`, and `std::nullopt` while the game must play on. Wins are
checked before draws.

**Side Effects:** updates the three streak counters.

### `runDatagen`

Run the generator to completion.

**When to use:** from the `datagen` subcommand ([main](main.cpp.md#main)).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `config` | `const DatagenConfig&` | The run (see [`DatagenConfig`](#struct-datagenconfig)). Assumed valid; [main](main.cpp.md#main) checks `threads >= 1`, `positions > 0`, `softNodes >= 1`. |

**Returns:** `int` — the process exit code: `0` on success, `1` if any output file cannot be
opened (reported on stderr before any thread starts).

**Side Effects:** creates `config.threads` workers and their output files, starts one
`std::jthread` per worker, and blocks in `reportUntilDone`. That loop polls the `running`
counter every 200 ms and prints a progress line every 10 s; the threads join when their
scope ends, then a final progress line is printed.

**Warnings:** workers check the target **between** games, so the total overshoots it by up
to one game per thread (a few hundred positions each). Output is appended: delete or rename
old files to start fresh. A run killed mid-write can leave a truncated last game in a file
(see [`Game::write`](viriformat.hpp.md#gamewrite)).
