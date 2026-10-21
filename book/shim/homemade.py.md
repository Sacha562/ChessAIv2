# Module: `homemade` (lichess-bot shim, `book/shim/homemade.py`)

The engine class lichess-bot loads. The LichessBotAPI compose stack bakes it
into the lichess-bot image at `/lichess-bot/homemade.py` (a `COPY --from=bookshim`
in that project's `docker/Dockerfile`, where `bookshim` is a compose build
context pointing at this folder), and it runs **inside the bot container**. It imports lichess-bot's own `lib` package, so it
cannot be imported or tested from this repo. Standard library + python-chess
only (both ship in the lichess-bot image).

It subclasses lichess-bot's `UCIEngine` (not `MinimalEngine`), so ChessAIv2
still runs as a normal UCI engine: pondering, `go_commands`, draw/resign logic,
and move commentary all keep working. The only change is in `search`: before
searching, it asks the book server ([server.py.md](../server.py.md#shim-endpoints))
for a move and plays it instantly if one comes back.

**Failure mode:** any server error or timeout (default 1 s) is logged as a
warning, and the engine searches. The book can never cost a game.

lichess-bot asks its own Polyglot and online books *before* calling `search`,
with no override hook. Those must be disabled in `config.yml` so this book
decides; see [\_\_init\_\_.py.md](../__init__.py.md#production-the-compose-stack-lichessbotapi)
for the full config.

## Source File

- **Code:** [homemade.py](homemade.py)

## Objects / Interfaces

| Constant | Value | Meaning |
|----------|-------|---------|
| `DEFAULT_BOOK_SERVER` | `http://book:8765` | The `book` service of the same compose stack. |
| `DEFAULT_TIMEOUT_S` | `1.0` | Per-move request timeout (`BookTimeout`). |
| `NOTIFY_TIMEOUT_S` | `2.0` | Timeout for game start/end notices. |
| `START_FENS` | `None`, `"startpos"`, standard FEN | Games from these start positions use the book. |

### `class ExampleEngine(MinimalEngine)`

Empty placeholder. Whenever lichess-bot loads a homemade engine it also imports
its own `test_bot/homemade.py`, which does `from homemade import ExampleEngine`.
Because this file *replaces* the stock `homemade.py`, it must define the name, or
lichess-bot crashes at start-up with `ImportError: cannot import name
'ExampleEngine'`.

### `class BookedEngine(UCIEngine)`

Selected with `engine.protocol: homemade` and `engine.name: BookedEngine`.
Options come from `engine.homemade_options`:

| Option | Required | Meaning |
|--------|----------|---------|
| `BookEnginePath` | yes | Path of the ChessAIv2 binary inside the container. |
| `BookServer` | no | Book server URL. |
| `BookTimeout` | no | Seconds to wait for a book answer. |
| *anything else* | — | Passed to the UCI engine (`Hash`, `Threads`, `Move Overhead`, …). |

| Field | Type | Description |
|-------|------|-------------|
| `book_server` | `str` | Base URL without a trailing slash. |
| `book_timeout` | `float` | Per-request timeout. |
| `game` | `model.Game \| None` | The lichess game. `None` during lichess-bot's start-up engine check, which disables the book. |
| `book_active` | `bool` | Standard chess from the normal start position and a real game. |

## Functions

### `BookedEngine.__init__`

Pops the three book options, then starts the UCI engine at `BookEnginePath`
with the remaining options. The engine `commands` lichess-bot builds from
`dir` + `name` are ignored, because `name` holds the class name. If the book is
active, it posts `game_start`, which pauses Stockfish analysis on the server.

**Warnings:** raises `KeyError` if `BookEnginePath` is missing; lichess-bot
reports it at start-up.

### `BookedEngine._post`

POSTs JSON and decodes the JSON reply.

**Returns:** the dict, or `None` on any exception (logged).

### `BookedEngine._book_move`

Sends `game_id`, the board's move stack (UCI), `my_color`, the opponent name and
the speed to `/api/shim/select`.

**Returns:** the move if it is legal, else `None`.

### `BookedEngine.search`

When lichess-bot passes `root_moves` as a list (online tablebase "suggest"
mode), the book is skipped. Otherwise:
- **Book move:** `self.engine.ping()` is sent first. Any new command makes
  python-chess stop a running ponder search, which would otherwise keep
  thinking about a stale position. A neutral `Mate(1)` score is recorded so the
  book move never triggers draw or resign decisions, and the move is returned
  without engine info.
- **No book move:** `UCIEngine.search`.

### `BookedEngine.__exit__`

Shuts the engine down via `UCIEngine.__exit__`, then posts `game_end`.
lichess-bot leaves this context exactly once per game, however the game ended,
so the server always resumes analysis and ingests the finished game.
