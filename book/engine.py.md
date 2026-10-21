# Module: `book.engine`

Thin wrapper around one Stockfish process (python-chess `SimpleEngine`), used by
the worker ([worker.py.md](worker.py.md)) for candidate generation and for the
per-game eval signal. Every score is converted to an **expected score** in
[0, 1] (win + draw/2). Expected scores average linearly and saturate in decided
positions, so a +9 blowout cannot dominate a mean the way centipawns would.

## Source File

- **Code:** [engine.py](engine.py)

## Objects / Interfaces

### `class LineEval`

Frozen dataclass: Stockfish's verdict on one root move, from the side to move's
point of view.

| Field | Type | Description |
|-------|------|-------------|
| `move` | `chess.Move` | Root move (first PV move). |
| `expected` | `float` | Expected score for the side to move after this move. |
| `cp` | `int \| None` | Centipawns (None for a mate score). |
| `mate` | `int \| None` | Mate distance in moves (None otherwise). |
| `depth` | `int` | Reported search depth. |

### `class Stockfish`

One engine process, configured with `Threads`, `Hash` and `UCI_ShowWDL` (when
offered). **Not thread-safe**: only the worker's analysis thread uses it.

## Functions

### `expected_score`

Expected score for `pov` from one analysis info dict: the engine's own `wdl`
when present, otherwise python-chess's Stockfish WDL model (`model="sf"`)
applied to the centipawn score at the board's ply.

### `Stockfish.__init__`

Starts the process at `path` and configures it.

### `Stockfish.close`

Sends `quit`.

### `Stockfish.top_moves`

MultiPV search at fixed `depth`.

**Returns:** `list[LineEval]`, best first by expected score. It may be shorter
than `multipv` when there are fewer legal moves.

### `Stockfish.eval_moves`

Evaluates exactly the given moves (UCI `searchmoves`, MultiPV = number of
moves) at fixed `depth`. Used for moves we played and for existing candidates
that MultiPV did not list.

**Returns:** `list[LineEval]`, best first; an empty list for no moves.

### `Stockfish.position_expected`

Single-PV search with a fixed `nodes` budget. Fixed nodes keep the signal
deterministic and independent of hardware load.

**Returns:** expected score for `pov`.
