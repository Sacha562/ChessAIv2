# Module: `book.config`

Static paths and network settings, plus every tunable setting with its default.
Static values are fixed for the server's lifetime and come from CLI arguments or
environment variables. Tunables are stored in the `settings` table
([db.py.md](db.py.md)), so the dashboard's **Settings** page
([static/app.js.md](static/app.js.md#settings)) can change them while the server
runs. Every consumer re-reads them per request or job via
[`load_settings`](#load_settings).

## Source File

- **Code:** [config.py](config.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Objects / Interfaces

### Path and network constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `PACKAGE_DIR` | directory of this file | Root of the `book` package (static files live under it). |
| `DEFAULT_DB_PATH` | `book/data/book.sqlite` | SQLite database (git-ignored). |
| `DEFAULT_STOCKFISH` | `~/Documents/programs/stockfish/stockfish-18-bmi2.exe` | Stockfish binary when run directly on Windows. The container overrides it with `BOOK_STOCKFISH` ([Dockerfile.md](Dockerfile.md)). |
| `DEFAULT_HOST` / `DEFAULT_PORT` | `127.0.0.1` / `8765` | Bind address of the server. |

### `DEFAULT_SETTINGS`

Every tunable and its default. Types are taken from the default value and
enforced by [`coerce_setting`](#coerce_setting).

| Setting | Default | Meaning |
|---------|---------|---------|
| `account` | `"SmikkelBot"` | lichess account whose games are ingested and whose side is "us". |
| `max_book_ply` | `30` | Book horizon (15 full moves). The book answers only before this ply; candidates are generated only for earlier positions. Replay ignores it. |
| `result_weight` | `0.5` | `w` in `s = w·result + (1−w)·eval` ([scoring.py.md](scoring.py.md)). |
| `window_start_moves` / `window_end_moves` | `10` / `20` | Eval window in full moves after the book exit (plies `exit+20 … exit+40`). |
| `k_stockfish` | `4.0` | Pseudo-games given to Stockfish's expected score as the prior of the pooled layer. |
| `k_pool` | `5.0` | Pseudo-games given to the pooled mean as the prior of the opponent layer. |
| `other_speed_weight` | `0.5` | Weight of this opponent's games at other speeds. |
| `exhaust_threshold` | `0.40` | Posterior mean below which a well-tried candidate counts as failing against an opponent. |
| `exhaust_min_games` | `3` | Weighted games against the opponent before a candidate can be exhausted. |
| `tier_gaps` | `[0.03, 0.07, 0.12, 0.18]` | Upper bound of the expected-score gap to the best move for tiers 0, 1, 2, 3. The last tier is filled only by widened analysis. |
| `candidate_always_ply` | `8` | Positions from game history before this ply are always analysed. |
| `candidate_min_visits` | `2` | Deeper positions from history are analysed only after being reached on our move this many times. |
| `analysis_depth` / `analysis_multipv` | `20` / `8` | Stockfish depth and MultiPV for normal candidate generation. |
| `widen_depth_bonus` / `widen_multipv` | `4` / `16` | Extra depth and MultiPV for a widen run. |
| `signal_nodes` | `1000000` | Stockfish nodes per eval-signal position. |
| `signal_stride` | `2` | Evaluate every 2nd ply (our side to move at the exit). |
| `signal_max_offset_plies` | `60` | Evaluate up to 60 plies past the exit (covers calibration windows). |
| `replay_enabled` | `true` | Replay won games against the same opponent. |
| `replay_min_result` | `1.0` | Minimum result of a game to replay (`0.5` would also replay draws). |
| `replay_same_speed_only` | `true` | Replay only games from the same speed bucket. |
| `ingest_interval_min` | `10` | Minutes between periodic ingests. |
| `pause_analysis_during_games` | `false` | Pause Stockfish completely while a lichess game is live. When false, it keeps running with `stockfish_threads_in_game` threads. |
| `stockfish_threads` / `stockfish_hash_mb` | `8` / `256` | Stockfish resources between games. Changing them restarts Stockfish. |
| `stockfish_threads_in_game` | `4` | Stockfish threads while a game is live, so the bot's engine (4 threads + ponder) keeps the rest of the 12 physical cores. |

### `class StaticConfig`

Frozen dataclass with the values fixed for the process lifetime.

| Field | Type | Description |
|-------|------|-------------|
| `db_path` | `Path` | SQLite file. |
| `stockfish_path` | `Path` | Stockfish executable used by the worker. |
| `host` | `str` | Bind address for `serve`. |
| `port` | `int` | Port for `serve`. |

**Used by:** [server.py.md](server.py.md), [worker.py.md](worker.py.md), [\_\_main\_\_.py.md](__main__.py.md)

## Functions

### `static_config_from_env`

Builds a [`StaticConfig`](#class-staticconfig). For each field the explicit
argument wins, then the environment variable (`BOOK_DB`, `BOOK_STOCKFISH`,
`BOOK_HOST`, `BOOK_PORT`), then the default.

**Returns:** `StaticConfig`.

### `coerce_setting`

Converts a raw value (JSON from the dashboard or the database) to the type of
`DEFAULT_SETTINGS[name]`:
- bool: strings `1/true/yes/on` count as true
- int / float: numeric conversion
- list (`tier_gaps`): a JSON string or a list, sorted ascending, every gap required to be positive
- anything else: `str`

**Returns:** the converted value.

**Warnings:** raises `KeyError` for an unknown setting name and `ValueError`
(or `TypeError`) for an unconvertible value. The server turns both into HTTP
400.

### `load_settings`

**Returns:** `dict[str, object]`: `DEFAULT_SETTINGS` overlaid with every
stored override. Stored keys that no longer exist in the defaults are ignored.

**Side Effects:** none (read-only query).

### `save_settings`

Validates each update with [`coerce_setting`](#coerce_setting), upserts it into
`settings`, and commits.

**Returns:** the full merged settings after the write.

**Warnings:** updates are applied one by one. If a later key is invalid, the
earlier ones are already executed but not committed, because the exception
propagates before `commit()`.
