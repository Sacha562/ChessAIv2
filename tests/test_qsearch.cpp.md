# Test: `test_qsearch.cpp`

Functional tests for [quiescence search](../src/search.hpp.md#searcherqsearch) and
move ordering, exercised end-to-end through the public [`Searcher`](../src/search.hpp.md#class-searcher).
Quiescence exposes no score accessor, so it is validated by the **move a fixed-depth
search makes**: it must grab free material, must **not** walk into a capture that
loses to the recapture (the horizon effect q-search exists to cure), and must still
find a forced mate. Every case is deterministic — fixed depth, single thread, a fresh
transposition table, no clock.

## Test Code

- **Test Code:** [test_qsearch.cpp](test_qsearch.cpp)
- **Module under test:** [search.hpp.md](../src/search.hpp.md) (with
  [see.hpp.md](../src/see.hpp.md) and [movepick.hpp.md](../src/movepick.hpp.md))
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Helpers

`bestMove(fen, depth)` runs a silent fixed-depth search from `fen` with a fresh 8 MB
TT and returns the best move as UCI text.

## Tests

### `captures a hanging queen`

A depth-2 search plays `e4xd5`, winning the undefended queen.

### `does not grab a pawn-defended pawn that loses the queen`

`Qd1xd5??` wins a pawn but hangs the queen to `...e6xd5`. A static depth-1 leaf sees
only `+100` and blunders; quiescence resolves the recapture, so the search must not
play `d1d5`.

### `wins a free undefended knight`

A depth-2 search plays `e4xd5`, capturing the undefended knight.

### `still finds a forced mate in one`

`Ra1-a8#` (the king boxed in by its own pawns) is found — a guard that adding
quiescence + ordering did not break mate detection.
