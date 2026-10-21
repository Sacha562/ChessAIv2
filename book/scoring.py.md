# Module: `book.scoring`

The learning signal of one game, and the calibration that checks which eval
window measures openings most reproducibly.

A game's result is a noisy verdict on its opening: the game is decided around
move 40, not move 8. So each game's signal blends the result with Stockfish's
opinion of the positions shortly after the book ended:

```
s = w · result + (1 − w) · eval
eval = mean expected score for us over plies exit+20, exit+22, …, exit+40
```

Here `exit` is the game's book-exit ply ([ingest.py.md](ingest.py.md#exit_ply)),
the window defaults to moves 10–20 after the exit, and `w = result_weight` (0.5).
Plies past the end of the game count as the final result. Until the worker has
evaluated a game, its signal is the result alone.

The per-ply expected scores are stored once (`signal_evals`, written by the
worker's [`run_signal`](worker.py.md#run_signal)) for every 2nd ply up to 60
plies past the exit. Changing the window or `w` therefore never needs Stockfish
again.

## Source File

- **Code:** [scoring.py](scoring.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Objects / Interfaces

### `class GameRow`

Frozen dataclass of the stored game fields the signal needs: `id`, `result`,
`exit_ply`, `n_plies`.

### Calibration constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `CALIBRATION_WIDTH_PLIES` | `20` | Each tested window spans 10 moves. |
| `CALIBRATION_STEP_PLIES` | `4` | Window starts tested every 2 moves. |
| `CALIBRATION_MIN_GROUP` | `4` | Games a group needs before it is split. |
| `CALIBRATION_MIN_GROUPS` | `3` | Groups needed before a correlation is reported. |
| `CALIBRATION_LINE_PLIES` | `8` | Plies that define "the same line" for grouping. |

## Functions

### `signal_plies`

Plies the worker evaluates for a game: `exit, exit+stride, …` up to
`exit + signal_max_offset_plies`, limited to plies inside the game.

**Returns:** `list[int]`.

### `eval_signal`

Mean expected score over plies `exit+start_off … exit+end_off` (step `stride`).
Plies at or past `n_plies` use the result.

**Returns:** `float`, or `None` if an in-game ply of the window has not been
evaluated yet.

### `blend`

`w·result + (1−w)·eval`; just the result when `eval` is `None`.

### `window_offsets`

The configured window as `(start, end)` ply offsets: `2 × window_*_moves`.

### `load_evals`

Batch-loads `signal_evals` for many games (chunks of 500 ids).

**Returns:** `dict[game_id, dict[ply, expected]]`, with an entry for every
requested id.

### `game_signals`

**Returns:** `dict[game_id, s]`, the blended signal of each game under the
current settings. Called by [selector.py](selector.py.md#_evidence) and
[reports.py](reports.py.md).

### `calibrate`

Split-half reliability of the signal for each candidate window.

1. Group all games by (opponent, speed, color, first 8 plies).
2. Keep groups with at least 4 games.
3. For each window (10 moves wide, starting every 2 moves from the exit), split
   each group's games by date into odd and even halves and take each half's
   mean signal.
4. Correlate the odd-half means with the even-half means across groups
   (Pearson r).

A higher r means the window ranks lines more reproducibly, which is what a
book's learning signal needs. The pure game result is reported as a baseline
row with `start_moves = None`.

**Returns:** `{"eligible_groups": int, "windows": [{"start_moves", "end_moves", "r", "groups"}, …]}`.
`r` is `None` when fewer than 3 groups have games in both halves.
Shown as a bar chart on the dashboard **Settings** page and printed by
`python -m book calibrate`.
