# Test: `test_book.py`

Unit tests for the opening-book package ([../__init__.py.md](../__init__.py.md)).
Strategy:
- **Pure-function tests** for the posterior math and the signal.
- **Database tests** against a fresh temporary SQLite file per test, fed with
  synthetic lichess export objects (`raw_game`), for ingest and the selector.
- **One end-to-end API test** through FastAPI's `TestClient`, with the worker
  disabled.

No network and no Stockfish are needed.

## Source File

- **Test Code:** [test_book.py](test_book.py)

## Related Code Documentation

- [../stats.py.md](../stats.py.md)
- [../scoring.py.md](../scoring.py.md)
- [../ingest.py.md](../ingest.py.md)
- [../selector.py.md](../selector.py.md)
- [../server.py.md](../server.py.md)
- [../db.py.md](../db.py.md), [../config.py.md](../config.py.md)

## How to Build & Run

From the repo root (Windows), with the package virtualenv
(`pip install -r book/requirements.txt`; `httpx2` is needed for the API test,
which is skipped without it):

```bash
book/.venv/Scripts/python -m unittest book.tests.test_book
```

## Tests

### `StatsTests`

- `test_shrink_without_evidence_is_prior`: with no games, the posterior mean equals the prior.
- `test_unknown_opponent_falls_back_to_pooled`: with no opponent games, the opponent-layer mean equals the pooled mean (the fallback requirement).
- `test_opponent_evidence_moves_posterior`: ten losses to one opponent pull a 55 % prior below 25 %.
- `test_zero_prior_weight_still_valid`: `k = 0` with no evidence still gives a sampleable Beta.
- `test_thompson_prefers_clearly_better_move`: Beta(90,10) vs Beta(10,90) is picked over 99 % of the time.
- `test_tier_unlocks_only_when_lower_tiers_exhausted`: fresh moves keep tier 0; exhausting tier 0 unlocks tier 1; exhausting everything sets the "all exhausted" flag.
- `test_few_games_never_exhaust`: fewer than `min_games` never counts as exhausted.

### `ScoringTests`

- `test_window_past_game_end_uses_result`: a window beyond the last ply equals the result.
- `test_missing_in_game_eval_is_pending`: an unevaluated in-game ply makes the eval `None`.
- `test_window_mean`: averaging over the window plies.
- `test_blend`: the weighted blend, and the result-only fallback.
- `test_signal_plies_start_at_exit`: evaluated plies start at the exit, step 2, inside the game.

### `IngestTests`

- `test_parse_result_from_our_side`: color, result and lowercase opponent id when we are black and win.
- `test_skips_variants_and_aborts`: chess960 and aborted games are rejected.
- `test_store_queues_candidates_and_signal`: storing a game queues both job kinds and records our moves as `by_us`.
- `test_deep_one_off_positions_not_queued`: the visit policy queues only shallow positions until a deeper one repeats; `rebuild_candidate_queue` re-plans the same set.
- `test_exit_inferred_without_log`: without book events, exit = `min(max_book_ply, n_plies)`.
- `test_exit_from_served_log`: a served first move puts the exit at our second move (ply 2).

### `SelectorTests`

- `test_book_pick_from_candidates`: a single tier-0 candidate is served with source `book`.
- `test_locked_tier_not_played`: a tier-1 move is never chosen while tier 0 is fresh (20 seeds).
- `test_miss_queues_position`: no candidates means a `miss` and a priority-0 `candidates` job.
- `test_replay_won_game`: a won game with a matching prefix is replayed.
- `test_no_replay_of_lost_game_or_other_opponent`: losses and other opponents' wins are not replayed.
- `test_horizon`: at `max_book_ply` the book returns `horizon`.
- `test_not_our_turn`: a request on the opponent's move returns no move.
- `test_opponent_losses_unlock_next_tier`: five losses with tier-0 `e4` against one opponent unlock tier 1 for that opponent only; a newcomer still gets tier 0.

### `ServerTests`

- `test_add_manual_candidate_and_read_position`: through the HTTP API:
  1. a SAN manual candidate (`Nf3`) is stored as UCI `g1f3`, source `manual`
  2. an illegal move answers 400
  3. the shim endpoint then serves the manual move
