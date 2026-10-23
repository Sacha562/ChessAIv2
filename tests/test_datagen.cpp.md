# Test: `test_datagen.cpp`

Unit tests for [datagen](../src/datagen.hpp.md)'s game adjudication
([`Adjudicator`](../src/datagen.hpp.md#class-adjudicator)). Strategy: feed scripted
white-relative score sequences into an `Adjudicator` and check exactly on which ply it
calls a win or a draw — streak lengths, streak resets, which side wins, and the minimum
game ply for draws. All thresholds are read from the class's own constants (`WIN_SCORE`,
`WIN_PLIES`, `DRAW_MIN_PLY`, `DRAW_SCORE`, `DRAW_PLIES`), so the tests follow a re-tune.

The game loop, threading, and file output are not unit-tested here. They are checked end
to end by a real `chessai datagen` run decoded by the `viriformat` crate and trained on
through bullet (see [datagen.hpp.md](../src/datagen.hpp.md)).

## Test Code

- **Test Code:** [test_datagen.cpp](test_datagen.cpp)
- **Module under test:** [datagen.hpp.md](../src/datagen.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `a win is called after WIN_PLIES consecutive decided plies`

`WIN_PLIES − 1` plies at `+WIN_SCORE` call nothing; the next decided ply returns
`WhiteWin`. The mirror sequence at `−WIN_SCORE` returns `BlackWin`.

### `an interrupted winning streak starts over`

A ply one centipawn under `WIN_SCORE` resets the streak: a full `WIN_PLIES` run must then
follow before `WhiteWin` is returned.

### `a draw needs DRAW_PLIES level plies past DRAW_MIN_PLY`

Twenty level (`0` cp) plies that all fall before `DRAW_MIN_PLY` never call a draw. Past it,
`DRAW_PLIES − 1` plies at `±DRAW_SCORE` call nothing and the next level ply returns
`Draw`, so the `≤ DRAW_SCORE` bound is inclusive on both sides.

### `a non-level score breaks the draw streak`

After `DRAW_PLIES − 1` level late plies, a ply at `DRAW_SCORE + 1` calls nothing and
resets the streak, so the following level ply does not call a draw either.
