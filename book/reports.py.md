# Module: `book.reports`

Read models for the dashboard ([static/app.js.md](static/app.js.md)). Each
function takes an open connection and returns JSON-ready dicts and lists. Every
position key is sent as a **string**, because JavaScript numbers cannot hold a
signed 64-bit integer exactly. Served by [server.py](server.py.md#dashboard-endpoints).

## Source File

- **Code:** [reports.py](reports.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md) (position view via the API test)

## Objects / Interfaces

| Constant | Value | Meaning |
|----------|-------|---------|
| `RECENT_EVENTS` | `30` | Book decisions on the overview. |
| `RECENT_GAMES` | `50` | Games listed per opponent. |
| `DETERMINISM_MAX_PLY` | `40` | Determinism is measured on opponent moves before ply 40. |
| `LINE_PLIES` | `6` | Plies that define a "line" on the opponent page. |
| `QUEUE_LIMIT` | `200` | Jobs listed on the queue page. |

## Functions

### `overview`

**Returns:** a dict with these keys:
- `totals`: `games`, `opponents`, `score`
- `speeds`: per speed, `games`, `score` and `opponents`
- `queue`: `{kind: {status: count}}`
- `coverage`: analysed positions, enabled candidates, manual candidates
- `events`: the last 30 `book_events`
- `last_ingest_at`

The server adds the worker snapshot.

### `_determinism`

Over positions where the opponent moved at least twice (before ply 40), the
share where they always chose the same move. Near 100 % means a deterministic
bot, whose won games are worth replaying.

**Returns:** `(share | None, number of repeated positions)`.

### `opponents`

One row per opponent:
- `opponent` (id), `name`, `is_bot`
- `games`, `score`, `wins` (games replay can use), `last_played`
- `speeds`: per speed, `games` and `score`
- `determinism`, `repeated_positions`

Sorted by games, descending.

### `opponent_detail`

For one opponent, returns a dict with:
- **`summary`:** one row per (speed, color) with `games`, W/D/L counts, mean
  blended signal, and mean exit ply.
- **`lines`:** the 20 most played 6-ply lines, each with UCI `moves`, `san`,
  `games`, `score` and `signal`.
- **`games`:** the 50 most recent games, each with id, date, speed, color,
  result, status, exit ply and source, signal, and opening SAN (first 10 plies).
- **`determinism`** and **`repeated_positions`**.

### `_played_here`

Moves actually played in a position, by us and by opponents. Per move: `games`,
our mean result, and `opponent_games` (against the selected opponent and speed).

### `position`

The explorer's view of the position after `moves`, built for `opponent` and
`speed`. An empty opponent gives the pooled view. Returns:
- **Position:** `fen`, `key` (string), `ply`, `san_path` (space-separated SAN of
  `moves`), `turn`, `in_horizon`, `moves`.
- **`candidates`:** for each candidate, `move`, `san`, `tier`, `source`,
  `enabled`, Stockfish fields, pooled n / raw mean / posterior mean, opponent n /
  raw mean / posterior mean, `alpha` / `beta`, `exhausted`, `playable`, and
  `pick` (Thompson probability among playable candidates).
- **`legal`:** every legal move as `{move, san}`.
- **Tier verdict:** `tier_limit`, `all_exhausted`, `widened`.
- **Analysis:** `analysis` (the `positions` row, or null) and `jobs` (queue rows
  for this key).
- **`played`:** from [`_played_here`](#_played_here).

**Warnings:** raises `ValueError` on illegal moves. The server validates them
first and answers 400.

### `queue`

Running and queued jobs first (by priority), then the latest finished or failed
ones, up to 200.
