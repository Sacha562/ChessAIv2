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
- [../prep.py.md](../prep.py.md)
- [../opponent_model.py.md](../opponent_model.py.md)
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
- `test_deep_one_off_positions_not_queued`: for lost games, the visit policy queues only shallow positions until a deeper one repeats; `rebuild_candidate_queue` re-plans the same set.
- `test_won_game_queues_every_book_position`: a won game queues all of our positions before `max_book_ply`, deep ones included, so its replay moves get evals.
- `test_exit_inferred_without_log`: without book events, exit = `min(max_book_ply, n_plies)`.
- `test_exit_from_served_log`: a served first move puts the exit at our second move (ply 2).

### `SelectorTests`

- `test_book_pick_from_candidates`: a single tier-0 candidate is served with source `book`.
- `test_locked_tier_not_played`: a tier-1 move is never chosen while tier 0 is fresh (20 seeds).
- `test_miss_queues_position`: no candidates means a `miss` and a priority-0 `candidates` job.
- `test_replay_won_game`: a won game with a matching prefix is replayed when its move is within the eval gate; `detail.loss` holds the gap to the best move.
- `test_replay_gate_skips_losing_move`: a won game's move rated 0.10 below the best is not replayed, and the book move is played instead.
- `test_replay_gate_falls_back_to_older_win`: when the newest win's move fails the gate, an older win with a good move is replayed.
- `test_unevaluated_replay_skipped_and_queued`: a replay move without an eval falls back to the book, and the position is re-queued for analysis at priority 0.
- `test_no_replay_of_lost_game_or_other_opponent`: losses and other opponents' wins are not replayed.
- `test_horizon`: at `max_book_ply` the book returns `horizon`.
- `test_not_our_turn`: a request on the opponent's move returns no move.
- `test_opponent_losses_unlock_next_tier`: five losses with tier-0 `e4` against one opponent unlock tier 1 for that opponent only; a newcomer still gets tier 0.

### `PrepTests`

- `test_prep_beats_replay_and_book`: with a candidate and a replayable win both present, the prep move is played with source `prep`.
- `test_prep_is_opponent_specific`: another opponent gets the normal book move.
- `test_prep_ignores_horizon`: prep applies beyond `max_book_ply`.
- `test_prep_line_sets_only_our_moves`: a Black line prepares only Black's moves; SAN rendering (`1.e4 d5 2.Ke2 dxe4`); removal.
- `test_illegal_prep_rejected`: an illegal move raises `ValueError`.
- `test_prep_counts_as_served_for_exit`: a `prep` event moves the book exit like a `book` event.

### `OpponentModelTests`

Uses `bongcloud_history` (an opponent playing 1...e5 2...Ke7 and 1.e4 2.Ke2)
and `FakeStockfish` (scores looked up by board FEN).

- `test_ply_patterns_found`: the four Bongcloud plies are detected as patterns.
- `test_inconsistent_ply_is_not_a_pattern`: plies they never play, or play inconsistently, are not patterns.
- `test_history_beats_pattern`: an actually-seen reply in this exact position beats the ply pattern; a new position falls back to the pattern.
- `test_recent_switch_outweighs_long_history`: after six 1.e4 games and three 1.e3 games, recency decay predicts 1.e3; with decay off (`half_life` 0) it predicts 1.e4 at 6/9.
- `test_dropped_habit_stops_being_a_pattern`: after twelve 2.Ke2 games, two deviating games keep the `("black", 2)` pattern; a third drops it, although 12/15 still meets the 80 % share.
- `test_deepen_deletes_value_of_lost_prediction`: a stored 1.e3 d5 value predicted from the dropped 2.Ke2 habit is deleted once deepen can no longer predict a reply there.
- `test_black_roots_cover_old_and_new_first_move`: two games into a switch, both 1.e4 and 1.e3 are deepen roots; with a high `model_root_min_prob` only the most likely one is.
- `test_deepen_scores_moves_against_predicted_reply`: `deepen` scores 2.f4 by the position after the predicted 2...Ke7; a second run with unchanged predictions makes no Stockfish calls.
- `test_model_reweights_and_promotes_for_that_opponent_only`: a modelled tier-2 move becomes playable and outranks the tier-0 move against that opponent, but stays locked for others.
- `test_predictable_opponent_auto_queues_deepen`: storing games of a predictable opponent queues one `deepen` job.

### `ServerTests`

- `test_add_manual_candidate_and_read_position`: through the HTTP API:
  1. a SAN manual candidate (`Nf3`) is stored as UCI `g1f3`, source `manual`
  2. an illegal move answers 400
  3. the shim endpoint then serves the manual move
  4. `POST /api/prep/line` with SAN (`e4 e5 f4 Ke7`, White) prepares `e2e4` and `f2f4`
  5. the shim serves `f2f4` with source `prep` for that opponent, even at another speed
