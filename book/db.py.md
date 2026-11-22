# Module: `book.db`

SQLite storage for the whole book: games, per-ply position keys, Stockfish
candidates, the analysis queue, per-ply eval-signal scores, the log of served
book moves, settings, and bookkeeping metadata. Only the book server
process ([server.py.md](server.py.md)) and the CLI ([\_\_main\_\_.py.md](__main__.py.md))
open the file. In production it sits on the `book` container's `/data` volume
([Dockerfile.md](Dockerfile.md)), and lichess-bot talks HTTP, so SQLite locking
never crosses a container boundary. WAL journaling lets the dashboard read while the
worker writes. Only one connection can write at a time, so every writer commits
before any slow step (a Stockfish search, a network read). A held write lock
would otherwise stall the shim's live move requests.

## Source File

- **Code:** [db.py](db.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Objects / Interfaces

### Schema (`SCHEMA`)

| Table | Key | Holds |
|-------|-----|-------|
| `games` | `id` (lichess game id) | One finished game from our side: `speed`, `rated`, `color` (`white`/`black`), `opponent` (lowercase lichess id), `opponent_name`, `opponent_rating`, `opponent_is_bot`, `result` (1 / 0.5 / 0 for us), `status`, `n_plies`, `moves` (space-separated UCI), `exit_ply` + `exit_source` (`served`/`inferred`, see [ingest.py.md](ingest.py.md#exit_ply)), `created_at` (lichess ms), `ingested_at` (s). |
| `game_moves` | (`game_id`, `ply`) | Position key **before** each ply, the move played, and `by_us`. Indexed on (`key`, `by_us`); this is the evidence table behind every statistic. |
| `positions` | `key` | FEN, Stockfish `best_expected`, `analysed_depth`, `analysed_multipv`, `analysed_at` (NULL = never analysed), `widened` (0/1). |
| `candidates` | (`key`, `move`) | Playable book moves: `tier` (99 = outside every tier), `source` (`engine` = MultiPV, `ours` = a move we played, evaluated with searchmoves, `manual` = added on the dashboard), Stockfish `sf_cp` / `sf_mate` / `sf_expected` / `sf_depth`, `enabled`, `added_at`. |
| `analysis_queue` | `id`; unique (`kind`, `target`) | Jobs: `kind` ∈ `candidates`, `widen` (target = position key as text), `signal` (target = game id), `deepen` (target = opponent id); `fen`, `priority` (lower runs first), `status` ∈ `queued`/`running`/`done`/`failed`, `error`, timestamps. |
| `signal_evals` | (`game_id`, `ply`) | Stockfish expected score for us at that ply ([scoring.py.md](scoring.py.md)). |
| `book_events` | `id` | Every live decision the book took: `game_id`, `ply`, `key`, `move` (NULL on a miss), `source` ∈ `prep`/`book`/`replay`/`miss`, `opponent`, `speed`, `created_at`. |
| `opponent_model` | (`opponent`, `key`, `move`) | Model value of our move vs an opponent's predicted reply: `expected`, `reply`, `reply_prob`, `source`, `replies` signature, `updated_at` ([opponent_model.py.md](opponent_model.py.md)). |
| `prep_moves` | (`opponent`, `key`) | Per-opponent prepared moves: UCI path `moves`, prepared `move`, `note`, `created_at` ([prep.py.md](prep.py.md)). |
| `settings` | `name` | JSON-encoded overrides of [`DEFAULT_SETTINGS`](config.py.md#default_settings). |
| `meta` | `name` | `last_created_at` (ingest cursor, lichess ms) and `last_ingest_at` (s). |

### Constants

| Name | Value | Meaning |
|------|-------|---------|
| `BUSY_TIMEOUT_S` | `30.0` | Default time a write waits for another connection's write lock. |

## Functions

### `position_key`

Polyglot Zobrist hash of a `chess.Board`, mapped to a **signed** 64-bit
integer so SQLite can store it in an `INTEGER` column. Polyglot hashing ignores
the move counters and includes the en-passant file only when a capture is
legal, so transpositions share a key.

**Returns:** `int` in `[-2^63, 2^63)`.

**Warnings:** JavaScript cannot represent this exactly. Every API response
sends keys as strings ([reports.py.md](reports.py.md)).

### `now`

Current Unix time in whole seconds.

### `connect`

Opens or creates the database at `path` (creating parent directories), sets
`row_factory = sqlite3.Row`, enables WAL and foreign keys, and applies
`SCHEMA` (idempotent `CREATE … IF NOT EXISTS`).

**Parameters:** `path`; `busy_timeout_s` (default
[`BUSY_TIMEOUT_S`](#constants)), how long a write waits for another
connection's write lock.

**Returns:** a `sqlite3.Connection` created with `check_same_thread=False`.

**Errors:** a write still locked after `busy_timeout_s` raises
`sqlite3.OperationalError` ("database is locked").

### `session`

Context manager that yields a fresh [`connect`](#connect) connection (same
`busy_timeout_s`) and closes it afterwards. The server uses one per request; the
worker uses one per step.

### `get_meta` / `set_meta`

Read or upsert one `meta` value. `set_meta` does not commit.

### `enqueue`

Queues a job keyed by (`kind`, `target`):
- **new job:** inserted as `queued`
- **already `queued` or `running`:** keeps its status and takes the lower of the two priorities
- **`done` or `failed`:** left alone, unless `force=True` (a dashboard "Analyse now" or a new manual candidate), which re-queues it and clears its error

`fen` is filled in if given.

**Side Effects:** writes `analysis_queue`; the caller commits.
