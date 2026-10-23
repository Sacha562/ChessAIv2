# Test: `test_search.cpp`

Functional tests for the **Phase 1c main-search extensions** —
[singular extensions](../src/search.hpp.md#searchersearch) — the **Phase 1d whole-node
forward pruning** (razoring, ProbCut), and the **Phase 2 datagen hooks** (the soft node
budget and the root-score getter), exercised through the public
[`Searcher`](../src/search.hpp.md#class-searcher). Singular extensions change how the
search *allocates depth*, not the result on a clear tactic, so there is no table to assert
against directly; the suite validates them behaviourally instead:

1. the engine still finds the forcing move in known tactical / mate positions with
   `UseSingular` **on**;
2. toggling `UseSingular` never changes the move it settles on for those forced positions
   (a correctness-regression guard that the verification search and its `excluded`-move
   plumbing do not corrupt the result);
3. lowering `SingularMinDepth` so the singular path fires heavily still leaves the forced
   move intact.

All searches are deterministic — fixed depth, single thread, no clock, a fresh TT per
call. A local `bestMove(fen, depth, tp)` helper runs a silent fixed-depth search under a
given [`Tunables`](../src/search.hpp.md#struct-tunables) (with a local
[`SearchState`](../src/search.hpp.md#enum-class-searchstate) left `Running`), and `singular(on)` builds a
`Tunables` with singular extensions toggled. Fixed FENs (a back-rank mate, a hanging
queen, a free knight) have a single forcing best move and are searched at depth ≥ 8 so the
default trigger depth (`SingularMinDepth = 8`) is reached.

## Test Code

- **Test Code:** [test_search.cpp](test_search.cpp)
- **Module under test:** [search.hpp.md](../src/search.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `singular search still finds the back-rank mate`

With singular extensions on, a depth-8 search plays `Ra1-a8#` (the king boxed in by its
own pawns) — the extension machinery does not lose a mate.

### `singular search still wins the hanging queen`

With singular extensions on, a depth-8 search plays `e4xd5`, grabbing the undefended
queen.

### `toggling singular does not change a forced tactic`

For each of the back-rank mate, hanging queen, and free-knight positions, the depth-8 best
move is identical with `UseSingular` on and off — extensions reallocate depth but do not
flip the choice on a forced tactic.

### `a low singular trigger depth still solves the forced tactic`

With `SingularMinDepth = 4` (so the verification search fires on nearly every node), the
hanging queen and back-rank mate are still solved — a guard that the verification search
and its exclusion plumbing stay correct under heavy use.

### `singular search is deterministic`

Two identical depth-9 singular searches of the same position return the same move — the
extension path introduces no non-determinism.

### `razoring and ProbCut leave a forced tactic intact`

Both Phase 1d whole-node prunes ship **off**; turning them on reshapes the tree but must
not change the verdict. The hanging-queen and free-knight positions return the same best
move with `UseRazoring`/`UseProbCut` on as with the shipped defaults.

### `an eager razoring gate still wins the hanging piece`

With `RazorBase = 600` and `RazorMargin = 400` the razoring gate fires on almost every
shallow node; the q-search verification must still refuse to prune a winning capture, so
`e4d5` survives in both positions. This is the test that would catch a razoring layer that
returns a fail-low score without verifying it.

### `an eager ProbCut still wins the hanging piece`

With `ProbCutMinDepth = 3` and `ProbCutMargin = 50` the shallow capture search runs
constantly; the forced capture must still be the move played — a guard on the SEE
threshold, the q-search screen, the reduced-depth verification, and the `BOUND_LOWER`
store that follows a successful cut.

### `a soft node budget finishes the iteration that crosses it`

The Phase 2 [datagen](../src/datagen.hpp.md) budget: a startpos search with
[`Limits::softNodes`](../src/search.hpp.md#struct-limits) `= 5000` and a hard `nodes` cap
of 1,000,000 must spend **at least** the soft budget (it stops only at an iteration
boundary, never before) and stay far below the hard cap (the soft stop, not the hard
abort, ends it).

### `score reports the last completed iteration`

[`Searcher::score`](../src/search.hpp.md#searcherscore) after a depth-4 search of the
back-rank mate is exactly `mate_in(1)`. Searching the hanging-queen position next with the
**same** `Searcher` gives a positive non-mate score (`exd5` leaves a lone pawn against a
bare king), proving the previous search's score is replaced rather than carried over.
