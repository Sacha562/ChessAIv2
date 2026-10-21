# Module: `book.ingest`

Turns exported lichess games ([lichess_api.py.md](lichess_api.py.md)) into book
data. For each game it:
- stores one `games` row
- stores one `game_moves` row per ply ([db.py.md](db.py.md#schema-schema)), which is all the evidence the statistics use
- works out where the game left the book
- queues Stockfish work: one eval-signal job for the game, and candidate generation for positions inside the book horizon that are worth analysing

It is called by the worker's scheduler ([worker.py.md](worker.py.md)) every
`ingest_interval_min` minutes and shortly after each game ends, and by
`python -m book ingest` / `requeue` ([\_\_main\_\_.py.md](__main__.py.md)).

## Source File

- **Code:** [ingest.py](ingest.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Objects / Interfaces

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `SKIPPED_STATUSES` | `created, started, aborted, noStart, unknownFinish` | Games with no usable result. |
| `SPEED_ORDER` | `ultraBullet … correspondence` | lichess speed buckets, fastest first (dashboard speed list). |
| `PRIORITY_MISS` | `0` | Live book miss: analyse first, since the position is being played. |
| `PRIORITY_MANUAL` | `1` | Dashboard "Analyse now" and new manual candidates. |
| `PRIORITY_WIDEN` | `5` | All tiers exhausted against an opponent. |
| `PRIORITY_CANDIDATE_BASE` | `10` | History position at ply *p* gets `10 + p`, so shallow positions go first. |
| `PRIORITY_SIGNAL` | `60` | Per-game eval signal; runs after candidate work. |

### `class ParsedGame`

Frozen dataclass: one finished game from the bot's side.

| Field | Type | Description |
|-------|------|-------------|
| `id` | `str` | lichess game id. |
| `created_at` | `int` | lichess `createdAt` (ms). |
| `speed` | `str` | lichess speed bucket. |
| `rated` | `bool` | Rated game. |
| `color` | `chess.Color` | Our color. |
| `opponent` | `str` | Opponent's lowercase lichess id (the identity key everywhere). |
| `opponent_name` | `str` | Display name. |
| `opponent_rating` | `int \| None` | Rating at game time. |
| `opponent_is_bot` | `bool` | Opponent has the BOT title. |
| `result` | `float` | 1, 0.5 or 0 from our side. |
| `status` | `str` | lichess termination status. |
| `moves` | `tuple[chess.Move, ...]` | Moves from the standard start position. |

## Functions

### `parse_game`

Converts one export object. Returns `None` for variants, custom start
positions (`initialFen`), statuses in `SKIPPED_STATUSES`, games the account did
not play, and move lists that fail to replay.

**Returns:** `ParsedGame | None`.

### `our_plies`

`range` of the plies on which `color` moves (even plies for white, odd for
black).

### `exit_ply`

Where the game left the book. Returns `(ply, source)`:
- **`'served'`:** `book_events` has `book`/`replay` rows for the game. The exit is our first ply the book did not supply, or `n_plies` if it supplied every move.
- **`'inferred'`:** no log (games from before the book existed, or the server was down). The exit is `min(max_book_ply, n_plies)`.

The eval-signal window is measured from this ply ([scoring.py.md](scoring.py.md)).

### `store_game`

Inserts or replaces one game. Deletes and re-inserts its `game_moves`, queues
candidate analysis for each of our positions before `max_book_ply` that passes
[`worth_analysing`](#worth_analysing), and queues the game's `signal` job.

**Side Effects:** writes `games`, `game_moves`, `positions`, `analysis_queue`;
the caller commits.

### `worth_analysing`

Filter for positions taken from game history: always true below
`candidate_always_ply`; deeper positions need at least `candidate_min_visits`
`game_moves` rows on our move at that key. This keeps one-off positions deep in a
game from flooding the queue: on SmikkelBot's first ingest it cut 17,170
positions to 3,563. Live misses bypass the filter ([selector.py.md](selector.py.md)).

### `rebuild_candidate_queue`

Deletes every `queued` (not `running`) candidate job and re-queues from all
history under the current policy. For each key, the position is rebuilt from the
game where it first occurred (lowest ply); keys reached only by transposition at
a different ply are skipped. Already-analysed positions are not re-queued.
Commits.

**Returns:** number of positions queued. Used by `python -m book requeue` after
changing `max_book_ply` or the visit settings.

### `queue_candidates`

Queues a `candidates` job for `board` (priority `10 + ply` unless given) unless
the position already has `analysed_at`. Inserts the `positions` row if needed.
The caller commits.

### `ingest_new`

Streams every game created after the stored cursor (`meta.last_created_at`),
stores the parseable ones, and commits after each game, moving the cursor even
past skipped games. Sets `meta.last_ingest_at` at the end.

**Returns:** number of games stored.

**Warnings:** propagates [`RateLimited`](lichess_api.py.md#objects--interfaces)
and network errors. Progress up to the failure is kept.
