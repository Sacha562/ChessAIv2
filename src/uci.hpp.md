# Module: `uci`

The UCI protocol front end. Reads commands from stdin, maintains the current board,
and dispatches to [search](search.hpp.md), [perft](perft.hpp.md), and
[bench](bench.hpp.md). The search runs on a separate thread so `stop` and
`go infinite` work while the engine keeps reading input. Invoked by
[main](main.cpp.md) as the default mode.

The public surface is a single free function, [`run_uci`](#run_uci); all state and
command handling live in a file-local `Engine` class (anonymous namespace).

## Source Files

- **Header (interface):** [uci.hpp](uci.hpp)
- **Source (implementation):** [uci.cpp](uci.cpp)

## How to Build & Run

`./chessai` with no arguments enters this loop (see [README.md](../README.md) and
[build.md](../build.md)). Point a GUI / cutechess / fastchess at the binary, or type
UCI commands directly. Supported: `uci`, `isready`, `ucinewgame`, `position`, `go`,
`stop`, `setoption`, `quit`, plus the debug helpers `d` (print board), `perft [d]` /
`perft test`, and `bench [d]`.

## Namespace

- Public API (`run_uci`) in namespace `engine`.
- `ENGINE_NAME`, `ENGINE_AUTHOR`, and the whole `Engine` class have internal
  linkage (anonymous namespace in `uci.cpp`).
- `uci::moveToUci` / `uci::uciToMove` used here resolve to **library** functions
  (`chess::uci::…` via `using namespace chess`), not engine code.

## Objects / Interfaces

### `class Engine` (internal)

File-local UCI session state and command loop. Not part of the public API — created
and run by [`run_uci`](#run_uci).

| Field | Type | Description |
|-------|------|-------------|
| `board_` | `Board` | Current position (starts at `STARTPOS`). |
| `stop_` | `std::atomic<bool>` | Abort flag shared with the [`Searcher`](search.hpp.md#class-searcher). |
| `searchThread_` | `std::thread` | Background thread running the current search. |
| `tt_` | [`TranspositionTable`](tt.hpp.md#class-transpositiontable) | The shared TT (default 16 MB), passed by reference to each [`Searcher`](search.hpp.md#class-searcher); persists across moves. |
| `hashMb_` | `int` | `Hash` option (MB); resizes `tt_` on change. |
| `threads_` | `int` | `Threads` option; accepted now, wired to Lazy SMP in Phase 1d. |
| `tunables_` | [`Tunables`](search.hpp.md#struct-tunables) | Live self-play-tunable knobs (time management, eval `Tempo`, q-search `DeltaMargin` / `EndgamePieces`, ordering toggles `UseKillers` / `UseHistory` / `UseCountermove` / `UseIIR`), copied into each [`Searcher`](search.hpp.md#class-searcher). |

## Functions

### `run_uci`

Construct an `Engine` and run its command loop until `quit`. This is the module's
entire public interface.

**Parameters:** none.

**Returns:** `void`.

**Side Effects:** reads stdin, writes stdout, spawns/joins the search thread.

### `Engine::loop` (internal)

Read lines from stdin, tokenize, and dispatch each UCI command. Every state-changing
command first calls [`stopSearch`](#enginestopsearch) so a running search is joined
before the board changes. Unknown tokens are ignored per the UCI spec.

### `Engine::handlePosition` (internal)

Parse `position [startpos | fen <fen>] [moves <m1> <m2> …]`, set `board_`, and apply
each move. Stops applying moves on the first malformed/illegal token. Reads from the
command's `std::istringstream&` (in/out — the stream is consumed).

### `Engine::handleGo` (internal)

Parse a `go` command into a [`Limits`](search.hpp.md#struct-limits), snapshot the
board and the current [`Tunables`](search.hpp.md#struct-tunables), and launch a
[`Searcher`](search.hpp.md#class-searcher) — sharing `tt_` by reference — on
`searchThread_`. The search runs on the snapshot copy so the main thread can keep
reading input.

### `Engine::handleSetOption` (internal)

Parse `setoption name <Name> value <Value>` and update `hashMb_` / `threads_` or a
field of [`tunables_`](search.hpp.md#struct-tunables). Recognized tunable options:
`TimeSoftPermille`, `TimeHardPermille`, `AssumedMovestogo`, `Tempo`, `DeltaMargin`,
and `EndgamePieces`, plus the Phase 1b selective-search toggles (`spin`, `0`/`1`) used to
A/B-isolate each signal's Elo: the step-1 ordering signals `UseKillers`, `UseHistory`,
`UseCountermove`, `UseIIR` (defaults reflect that A/B — `UseKillers`/`UseCountermove`
default `1`, `UseHistory`/`UseIIR` default `0`, deferred until LMR), and the pruning /
reduction / extension toggles `UseNMP`, `UseRFP`, `UseFutility`, `UseLMP`, `UseLMR`,
`UseCheckExt`, `UseAspiration` (all default `1`) with `AspirationDelta` (`spin`, default
`15`, the initial aspiration half-window in cp). Each is advertised in the `uci` reply as
a `spin` option so a self-play tuner (SPSA) can perturb it without a rebuild, and each
**clamped to its advertised `min`/`max`** on store so an out-of-range value (e.g. a raw
spin from a tuner) can never reach the search as an out-of-bounds int. The advertised `default`
for `Tempo`, `DeltaMargin`, and `EndgamePieces` matches the corresponding
[`Tunables`](search.hpp.md#struct-tunables) field, whose values were set from an SPSA
self-play tune. Setting `Hash` first calls [`stopSearch`](#enginestopsearch) (no
live search may hold the table) and then
[`tt_.resize`](tt.hpp.md#transpositiontableresize). Malformed integer values are
ignored (caught). Other option names are accepted and ignored for now.

`ucinewgame` clears the TT ([`tt_.clear`](tt.hpp.md#transpositiontableclear)) after
stopping any search, so a fresh game starts with an empty table.

### `Engine::stopSearch` (internal)

Signal the running search to stop (`stop_ = true`), join `searchThread_`, then reset
`stop_` to `false`. Idempotent and safe when no search is running. Also called from
the destructor to guarantee a clean join on shutdown.
