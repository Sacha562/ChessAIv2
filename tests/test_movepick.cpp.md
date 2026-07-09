# Test: `test_movepick.cpp`

Unit tests for [move ordering](../src/movepick.hpp.md) (`src/movepick.*`). The suite
mixes **structural invariants** that must hold for any position with direct
**MVV-LVA ranking** checks. The bucket invariant is asserted by re-deriving each
move's category from the same `isCapture` / [`seeGE`](../src/see.hpp.md#seege) signals
the scorer uses, then checking the ordered list never steps back up a category.

Since Phase 1b step 1, [`orderMoves`](../src/movepick.hpp.md#ordermoves) takes a
[`History`](../src/history.hpp.md), a `ply`, and a `prevMove`. These tests pass an
**empty** `History` (via a local `order()` helper) so they isolate the capture/quiet
**bucket structure** from the quiet signals — the killer / countermove / history
interactions live in [test_history.cpp.md](test_history.cpp.md).

## Test Code

- **Test Code:** [test_movepick.cpp](test_movepick.cpp)
- **Module under test:** [movepick.hpp.md](../src/movepick.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `orderMoves groups good captures, then quiets, then losing captures`

Over Kiwipete, a symmetric middlegame, and the start position, every move's category
(winning/equal capture = 2, quiet = 1, losing capture = 0) is non-increasing down the
ordered list — the three buckets never interleave.

### `orderMoves puts the TT move first`

Given a TT move that is otherwise ordered late (the last-generated start-position
move), `orderMoves` floats it to index 0.

### `mvvLva ranks by victim value, then by (cheaper) attacker`

In a position where a pawn can take a queen or a rook and a queen can also take the
rook: `PxQ > PxR` (most-valuable victim wins) and `PxR > QxR` (same victim, the
least-valuable attacker wins).
