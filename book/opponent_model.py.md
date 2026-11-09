# Module: `book.opponent_model`

Models how a specific opponent replies, and turns that into **per-opponent
weights** for our candidate moves. Nothing is forced.

**Why it exists.** The generic book scores every candidate by Stockfish's
verdict against *best* play. Against a predictable opponent that undersells the
moves that punish their habits. Bongaclang, for example, answers 1.e4 with
1...e5 and then plays 2...Ke7 whatever we do. 2.f4 is a mediocre tier-2 move
in general, but after the predicted 2...Ke7 it is worth +5. The model scores
each candidate *after the opponent's predicted reply* and feeds that score
into the opponent layer of the posterior ([selector.py.md](selector.py.md#analyse_position)).
Real results against the opponent keep correcting it, like any other evidence.

**How it relates to other parts.**
- **[replay](selector.py.md#replay_move):** replays a whole won game.
- **[prep](prep.py.md):** a hand-set forced move.
- **The model:** only changes weights, so Thompson sampling still decides.

## Source File

- **Code:** [opponent_model.py](opponent_model.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md) (`OpponentModelTests`)

## Objects / Interfaces

### `class Reply`

Frozen dataclass:
- **`move`:** UCI.
- **`prob`:** predicted probability.
- **`source`:**
  - `history`: the opponent's own recency-weighted reply frequencies in this exact position
  - `pattern`: the same move at this ply in their recent games

### Table `opponent_model` ([db.py.md](db.py.md#schema-schema))

Primary key (`opponent`, `key`, `move`). One row per our candidate `move` in
our position `key`:

| Column | Meaning |
|--------|---------|
| `expected` | Expected score for us after the predicted reply(ies), probability-weighted. |
| `reply` / `reply_prob` / `source` | The most likely predicted reply. |
| `replies` | Signature of the full prediction (`move:prob …`, probabilities rounded to `SIGNATURE_PROB_STEP` = 0.05), used as a cache key. |
| `updated_at` | When it was last computed. |

### Settings

Defined in [`DEFAULT_SETTINGS`](config.py.md#default_settings):

| Setting | Default | Role |
|---------|---------|------|
| `k_model` | `8.0` | Pseudo-games the model value is worth in the opponent layer. |
| `model_recent_games` | `20` | Recent games per our color used for ply patterns. Older habits fade out. |
| `model_half_life_games` | `3.0` | Reply history decay: a game this many games older (same color for us) counts half. `0` turns decay off. |
| `model_root_min_prob` | `0.2` | As Black, every first move of theirs at least this likely gets its own deepen root. |
| `model_pattern_min_games` / `model_pattern_share` | `3` / `0.8` | A ply is a pattern with ≥ 3 games and a top-move share of ≥ 80 %. |
| `model_pattern_max_ply` | `12` | Patterns are only looked for in the first 6 moves. |
| `model_pattern_break_games` | `3` | A pattern is dropped once their latest 3 games at that ply all deviate from it. |
| `model_nodes` | `2000000` | Stockfish nodes per evaluated position. |
| `model_branch` | `2` | Lines followed per position (best-scoring candidates). |
| `model_max_positions` | `40` | Our positions expanded per deepen run. |
| `model_promote_min` | `0.6` | A modelled move scoring at least this is playable above the unlocked tier. |
| `model_auto_determinism` / `model_auto_min_games` | `0.7` / `10` | Which opponents get an automatic deepen run after each ingested game. |

## Functions

### `determinism`

Share of repeated positions (before ply 40) where the opponent always chose the
same move.

**Returns:** `(share | None, repeated positions)`. Shown on the dashboard
([reports.py.md](reports.py.md#opponents)) and used by
[`queue_deepen_if_predictable`](ingest.py.md#queue_deepen_if_predictable).

### `ply_patterns`

`{(our color, ply): move}` for plies where the opponent's recent games (the
latest `model_recent_games` per our color) agree. Before November 2026, for
Bongaclang:
- `("black", 0)` → `e2e4` and `("black", 2)` → `e1e2`
- `("white", 1)` → `e7e5` and `("white", 3)` → `e8e7`

A ply whose latest `model_pattern_break_games` games all deviate from the top
move is skipped, even when the share still qualifies. Without that rule,
Bongaclang's 17-of-20 `e1e2` kept predicting 2.Ke2 after its new 1.e3, though
it had just played 2.Ne2, 2.d4 and 2.e3. One or two deviations keep the habit.

### `_ply_pattern`

The habitual move at one ply from the opponent's moves there, newest first:
`None` below `model_pattern_min_games` games, below `model_pattern_share`, or
when none of the latest `model_pattern_break_games` moves is the top move.

### `history_weights`

Recency-weighted counts of the opponent's legal moves in `board`. A game's age
is its rank among this opponent's games with the same color for us, newest
first. A move from a game `age` games old counts `0.5 ** (age / half_life)`,
so habits fade at the pace that opponent plays us with that color.

**Why it exists.** Plain counts never let go of an old habit. In November 2026
Bongaclang dropped 1.e4 2.Ke2 for 1.e3 and 1.d3, but its dozens of 1.e4 games
kept the model predicting 1.e4. With `model_half_life_games` = 3, three games
of a new move outweigh any length of older history.

### `predict_replies`

The opponent's predicted replies in `board` (opponent to move), most likely
first:
1. Their [recency-weighted](#history_weights) reply frequencies in this exact
   position, if they have been here (`history`).
2. Otherwise, the ply pattern if its move is legal here (`pattern`).
3. Otherwise, an empty list: no prediction.

### `model_values`

Stored model rows for (opponent, position), keyed by our move. Read by the
selector.

### `_score_candidate`

Our expected score after `move` and the predicted replies: a
probability-weighted Stockfish `position_expected` at `model_nodes`, or the
result for a finished game. If the stored row has the same reply signature, the
cached value is reused, so repeated deepen runs cost Stockfish time only where
the prediction changed. Signature probabilities are rounded to 0.05, so the
small drift recency decay causes after each game does not force a re-run.

**Returns:** `(expected, replies)`, or `None` when the reply cannot be
predicted. Nothing is stored in that case, because the model would add nothing
over the generic book, and a row stored from an earlier prediction is deleted.
Otherwise a habit the opponent dropped would keep weighting the move.

### `_position_value`

Stockfish expected score for us, or 1 / ½ / 0 for a finished game.

### `black_roots`

Positions after each first move the opponent plays as White with a predicted
probability of at least `model_root_min_prob`, most likely first. The most
likely move is always included. An opponent who mixes first moves gets every
main one modelled, and during a switch both the old and the new move are.

### `deepen`

Walks the predicted lines breadth-first:
- **Roots:** the start position for our White games, and every
  [`black_roots`](#black_roots) position for our Black games.
- **At each of our positions below `max_book_ply`:**
  1. Make sure it has candidates (`ensure_candidates`, which runs Stockfish
     candidate generation via [`run_deepen`](worker.py.md#run_deepen)).
  2. Score every enabled candidate.
  3. Continue along the `model_branch` best-scoring candidates with their most
     likely reply.

A line stops where the reply can't be predicted. At most `model_max_positions`
positions are expanded per run; each position commits.

**Returns:** positions expanded.
