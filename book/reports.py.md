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

Determinism figures come from [`opponent_model.determinism`](opponent_model.py.md#determinism).

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
- **`prep`:** this opponent's [prep entries](prep.py.md#list_prep).
- **`patterns`:** [`_patterns`](#_patterns).
- **`model_rows`** and **`model_updated_at`:** size and freshness of this
  opponent's [model](opponent_model.py.md).

### `_patterns`

The opponent's [ply patterns](opponent_model.py.md#ply_patterns) as rows of
`our_color`, `ply`, `move_number` and `move` (UCI).

### `_san_after`

SAN of a predicted reply after our candidate move, or `None`.

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
- **`prep`:** the prepared UCI move for this opponent here, or null (always null in the pooled view).
- **Model fields, per candidate:** `model_expected`, `model_source`, and
  `model_reply` (SAN of the predicted reply via [`_san_after`](#_san_after)).
  All null when the move is not modelled.

**Warnings:** raises `ValueError` on illegal moves. The server validates them
first and answers 400.

### `queue`

Running and queued jobs first (by priority), then the latest finished or failed
ones, up to 200.
