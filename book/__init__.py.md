# Package: `book` (opponent-aware learning opening book)

An opening book for **SmikkelBot** on lichess that learns, per opponent and per
lichess time-control bucket (`ultraBullet`, `bullet`, `blitz`, `rapid`,
`classical`, `correspondence`), which opening moves score well, and serves them
to lichess-bot. Against an opponent with no history it falls back to how each
move scores against all other opponents at the same speed. It also replays won
games against opponents who repeat themselves, and grows its candidate moves
from Stockfish as new positions are reached. A local web dashboard shows every
opponent, time control and position, and lets you add or override candidates.

This is [PLAN.md](../PLAN.md) Component 24, option **D**. It is developer-side
tooling: it never links into the engine binary, and the C++ engine stays a plain
UCI engine.

## Source File

- **Code:** [\_\_init\_\_.py](__init__.py) (package docstring only)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Architecture

One Docker Compose stack (`LichessBotAPI/docker-compose.yml`) runs two services:

```
lichess <──games/moves── [lichess-bot]  smikkelbot container
   │                       BookedEngine (shim/homemade.py) ──UCI──> /engines/chessai
   │ export API            │ POST http://book:8765/api/shim/{select,game_start,game_end}
   v                       v
 [book]  smikkelbot-book container: server.py (FastAPI) + worker.py (Stockfish 19)
   │      SQLite on named volume book-data (/data/book.sqlite)
   └──> dashboard published on 127.0.0.1:8765
```

- The **book service** ([Dockerfile.md](Dockerfile.md)) is the only process that
  opens the SQLite file. The bot talks to it over the compose network as
  `http://book:8765`.
- The **shim** ([shim/homemade.py.md](shim/homemade.py.md)) is baked into the
  lichess-bot image and subclasses lichess-bot's `UCIEngine`. Before each search
  it asks the book for a move and plays it instantly if there is one. Otherwise,
  or if the book is down, the engine searches as usual.
- The **worker** ([worker.py.md](worker.py.md)) runs Stockfish for candidate
  generation and for the per-game eval signal. While a game is live it drops to
  `stockfish_threads_in_game` threads, so the bot's engine keeps most of the CPU.

## Module map

| Module | Role |
|--------|------|
| [config.py](config.py.md) | Static paths, tunable settings and their defaults. |
| [db.py](db.py.md) | Schema, connections, position keys, the job queue. |
| [lichess_api.py](lichess_api.py.md) | Streams the bot's finished games from lichess. |
| [ingest.py](ingest.py.md) | Games → `games`/`game_moves`; queues analysis. |
| [engine.py](engine.py.md) | Stockfish wrapper; reports expected scores. |
| [scoring.py](scoring.py.md) | Per-game learning signal; window calibration. |
| [stats.py](stats.py.md) | Beta posteriors, Thompson sampling, tier rule. |
| [selector.py](selector.py.md) | Picks the book move for a live request. |
| [worker.py](worker.py.md) | Background Stockfish queue and periodic ingest. |
| [reports.py](reports.py.md) | Read models for the dashboard. |
| [server.py](server.py.md) | FastAPI app: shim API, dashboard API, static files. |
| [\_\_main\_\_.py](__main__.py.md) | CLI: `serve`, `ingest`, `requeue`, `calibrate`. |
| [shim/homemade.py](shim/homemade.py.md) | lichess-bot engine class (runs in Docker). |
| [static/index.html](static/index.html.md), [app.js](static/app.js.md), [style.css](static/style.css.md) | Dashboard frontend. |

## How it learns (summary)

1. **Signal per game.** `s = w·result + (1−w)·eval`, where `eval` is Stockfish's
   mean expected score (from WDL) over plies `exit+20 … exit+40` after the game
   left the book (moves 10–20). `w = 0.5` by default. See
   [scoring.py.md](scoring.py.md).
2. **Evidence per move.** Every game credits its `s` to each of our moves played
   in each position (keyed by Polyglot Zobrist hash, so transpositions merge).
3. **Layered posterior.** Stockfish prior → all *other* opponents at this speed
   (pseudo-count `k_stockfish`) → this opponent (pseudo-count `k_pool`; their
   games at other speeds count `other_speed_weight`). See
   [stats.py.md](stats.py.md).
4. **Thompson sampling** picks among playable candidates. See
   [selector.py.md](selector.py.md).
5. **Tiers.** Candidates are tiered by expected-score gap to Stockfish's best
   move. A higher tier unlocks *per opponent and speed* only when every lower
   candidate is exhausted against that opponent. When all are exhausted, a
   deeper and wider re-analysis is queued. See [worker.py.md](worker.py.md).
6. **Replay.** If we beat this opponent before in a game whose moves match the
   current game so far, that game's next move is played, for the whole game.

## How to Build & Run

### Production: the compose stack (`LichessBotAPI/`)

The stack builds from this checkout (`../ChessAIv2/book` for the book image,
`../ChessAIv2/book/shim` for the shim), but only at build time:

```bash
docker compose up -d --build
docker compose up -d --build book
docker compose logs -f book
```

The first command builds and starts everything; the second rebuilds just the
book after a code change. Open the dashboard at <http://127.0.0.1:8765>
(this machine only).

The worker ingests new games on start, every 10 minutes, and after each game,
then works through the analysis queue: about 6 s per candidate position at
depth 20 with 8 threads, shallow positions first. The database lives in the
`book-data` volume and survives rebuilds and `docker compose down`
(`down -v` deletes it).

lichess-bot's `config.yml` selects the shim and keeps its own books off:

```yaml
engine:
  dir: "/engines"                    # must exist; the binary path is in homemade_options
  name: "BookedEngine"
  protocol: "homemade"
  polyglot: {enabled: false}
  online_moves: {chessdb_book: {enabled: false}, lichess_cloud_analysis: {enabled: false},
                 lichess_opening_explorer: {enabled: false}}
  homemade_options:                  # protocol "homemade" reads these, not uci_options
    BookEnginePath: "/engines/chessai"
    BookServer: "http://book:8765"
    BookTimeout: 1.0
    Threads: 4
    Hash: 512
```

To play without the book, set `protocol: "uci"` and `name: "chessai"` again and
restart lichess-bot.

### Development: run directly on Windows

```bash
python -m venv book/.venv
book/.venv/Scripts/python -m pip install -r book/requirements.txt
book/.venv/Scripts/python -m book serve
book/.venv/Scripts/python -m unittest book.tests.test_book
```

This uses `book/data/book.sqlite` and the Windows Stockfish. Configuration:
- **Paths and binding:** `--db` / `BOOK_DB`, `--stockfish` / `BOOK_STOCKFISH`,
  `--host` / `BOOK_HOST`, `--port` / `BOOK_PORT`.
- **lichess token:** `LICHESS_TOKEN`, for the higher API rate limit.
- **Everything else:** under **Settings** in the dashboard
  ([config.py.md](config.py.md)).

Stop the compose `book` service first if you use port 8765. The dashboard has no
authentication; keep it bound to localhost.
