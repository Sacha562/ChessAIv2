# Module: `book.selector`

Chooses the book move for one live request from the lichess-bot shim
([shim/homemade.py.md](shim/homemade.py.md)) via
[`POST /api/shim/select`](server.py.md#shim-endpoints). Precedence:

1. **Replay:** if a game we won against this opponent (same color; same speed
   by default) has exactly the current move list as its prefix, play its next
   move. This works at any ply, beyond the book horizon too, until the opponent
   deviates. Deterministic bots repeat their games, so a win often repeats.
2. **Book:** before `max_book_ply`, Thompson-sample among the candidates
   playable against this opponent ([stats.py.md](stats.py.md)).
3. **Nothing:** the engine searches.

Every served move and every miss is written to `book_events`. That log is what
[`exit_ply`](ingest.py.md#exit_ply) uses to find each game's book exit, and
what the dashboard's *Recent book decisions* shows. A miss on a position
without candidates queues it for Stockfish at the highest priority, so the
book covers it next time.

[reports.py](reports.py.md) reuses [`analyse_position`](#analyse_position) for
the dashboard explorer, so the dashboard shows exactly what the selector sees.

## Source File

- **Code:** [selector.py](selector.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Objects / Interfaces

### `class Request`

| Field | Type | Description |
|-------|------|-------------|
| `game_id` | `str` | lichess game id (for the event log). |
| `moves` | `tuple[str, ...]` | UCI moves from the start position. |
| `color` | `chess.Color` | Our color. |
| `opponent` | `str` | Lowercase opponent id. |
| `speed` | `str` | lichess speed bucket. |

### `class Decision`

`move` (UCI or `None`), `source`, `detail` (dict). Sources:
- `replay`: `detail.game` is the replayed game's id
- `book`: `detail` holds `tier_limit` and `posterior_mean`
- `miss`
- `horizon`
- `not_our_turn`
- `invalid`: `detail.error` says why

### `class CandidateView`

One candidate with its evidence and posteriors:
- **Stored fields:** `move`, `tier`, `source`, `enabled`, `sf_expected`, `sf_cp`, `sf_mate`.
- **Evidence:** `pooled` and `opponent` ([`MoveStats`](stats.py.md#class-movestats)).
- **Posteriors:** `pooled_post` and `opponent_post` ([`Posterior`](stats.py.md#class-posterior)).
- **Flags:** `exhausted`, and `playable` (tier ≤ tier limit).

### `class PositionAnalysis`

`key`, `candidates` (list of `CandidateView`, legal moves only, ordered by tier
then Stockfish score), `tier_limit`, `all_exhausted`, `widened`.

## Functions

### `board_from_moves`

Replays UCI moves from the start position.

**Warnings:** raises `ValueError` on an illegal or malformed move.

### `_evidence`

Loads every game in which **we** played a move from `key`, computes each game's
signal ([`game_signals`](scoring.py.md#game_signals)), and splits it into:
- **pooled:** other opponents at the same speed, weight 1
- **opponent:** this opponent, weight 1 at the same speed and `other_speed_weight` at other speeds

This opponent's games never enter the pooled layer, so the fallback is "all
*other* opponents".

### `analyse_position`

Builds the [`PositionAnalysis`](#class-positionanalysis) for a board, opponent
and speed: candidates with layered posteriors, the per-opponent tier limit
([`unlocked_tier`](stats.py.md#unlocked_tier), computed over enabled candidates
only), and the exhausted/playable flags. An empty opponent means every game
counts as pooled, which is the dashboard's pooled view.

### `replay_move`

Finds won games against the opponent with our color, `result ≥ replay_min_result`,
more plies than played so far, and (when `replay_same_speed_only`) the same
speed. Most recent first; the first whose move prefix equals the current moves
wins.

**Returns:** `(next move UCI, game id)` or `None`.

### `_book_pick`

Runs [`analyse_position`](#analyse_position). If every tier is exhausted against
this opponent and the position was never widened, it queues a `widen` job. With
no playable candidate it logs a `miss` and queues the position
([`queue_candidates`](ingest.py.md#queue_candidates), priority 0). Otherwise it
Thompson-samples, logs `book`, and returns the move.

### `choose`

Entry point. Rebuilds the board (`invalid` on failure), returns `not_our_turn`
if we are not to move, tries replay (when `replay_enabled`), returns `horizon`
at or beyond `max_book_ply`, else [`_book_pick`](#_book_pick). It never raises
for a bad request.

**Side Effects:** inserts into `book_events` and may queue analysis jobs; commits.
