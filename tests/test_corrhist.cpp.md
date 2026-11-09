# Test: `test_corrhist.cpp`

Unit tests for [correction history](../src/corrhist.hpp.md) (`src/corrhist.*`) — the
learned correction to the static eval, read from five tables (pawn, non-pawn per colour,
minor, major, continuation). Strategy: drive a `CorrectionHistory` through its public
[`slots`](../src/corrhist.hpp.md#correctionhistoryslots) /
[`update`](../src/corrhist.hpp.md#correctionhistoryupdate) API and assert the observable
contract — an empty table corrects nothing, a residual moves the correction in its own
direction, each table is keyed by **its own piece subset** and the **side to move**,
[weights](../src/corrhist.hpp.md#struct-corrweights) switch tables' reads on and off, the
gravity update makes the tables **converge together** on the residual instead of
overshooting, every entry saturates inside
[`CORR_LIMIT`](../src/corrhist.hpp.md#corr_limit), a corrected eval never reaches the mate
range, and `clear()` resets.

Local helpers: `only(pawn, nonPawn, minor, major, cont)` builds a
[`CorrWeights`](../src/corrhist.hpp.md#struct-corrweights); `slotsOf(board, prevPiece,
prevTo)` hashes a position's [`CorrSlots`](../src/corrhist.hpp.md#struct-corrslots)
(`prevPiece` `-1` = no previous move); `teach()` applies the same residual repeatedly. The
FENs are the start position, the start position with the knights removed (identical pawns,
different pieces), the same position with Black to move, and a position with White's
e-pawn advanced (a different pawn skeleton, same pieces).

## Test Code

- **Test Code:** [test_corrhist.cpp](test_corrhist.cpp)
- **Module under test:** [corrhist.hpp.md](../src/corrhist.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `a fresh CorrectionHistory corrects nothing`

A default-constructed history reports `correction == 0` (even with a previous move in the
slots) and leaves a raw eval unchanged through
[`corrected`](../src/corrhist.hpp.md#correctionhistorycorrected) — the property that keeps
a fresh search (and `bench`) starting from the raw evaluation.

### `a residual moves the correction in its own direction`

Repeated positive residuals leave a positive correction (and `corrected(slots, 0)` equals
it); repeated negative residuals leave a negative one. This is the sign convention the
search depends on: `diff = searchScore − correctedEval`, so a search that keeps coming in
*above* the eval raises it.

### `the pawn table is keyed by the pawn structure, not the pieces`

With only the pawn table read, two positions with identical pawns but different pieces
share a [`pawnStructureKey`](../src/corrhist.hpp.md#pawnstructurekey) and therefore the
same learned correction (by design — a structural prior), while a position with an
advanced pawn hashes differently and starts uncorrected.

### `piece keys see their own pieces only`

Removing the knights changes the white [`nonPawnKey`](../src/corrhist.hpp.md#nonpawnkey)
and the [`minorKey`](../src/corrhist.hpp.md#minorkey) but not the
[`majorKey`](../src/corrhist.hpp.md#majorkey); a pawn move changes none of the piece keys;
and the two colours' non-pawn keys differ for the same position.

### `a table with weight 0 is never read`

With every weight `0` the correction stays `0` however much the tables learned; switching
the continuation weight back to `100` reveals what that table learned all along — weights
gate the **read**, not the learning.

### `the continuation table is keyed by the previous move`

With only the continuation table read, a residual taught under previous move (piece 3,
square 20) is read back under that move, not under square 21, and not when there is no
previous move (`prevPiece = -1`).

### `every table is keyed by the side to move`

Teaching the White-to-move slots leaves the Black-to-move slots of the same position
(same keys) at zero in every table. Scores are side-to-move relative, so sharing one
bucket across colours would fight itself.

### `the tables converge on the residual together`

Repeatedly teaching the gap left after the current correction (`30 − correction`), as the
search does, settles the **summed** correction within ±3 cp of 30 cp. This is the
property the gravity update against the corrected eval exists for: six entries learn one
residual between them rather than each claiming all of it.

### `each entry saturates inside CORR_LIMIT`

Hundreds of huge residuals at maximal depth leave the summed correction at or below six
entries' worth of `CORR_LIMIT / CORR_GRAIN` (±64 cp each), and the pawn table read alone
at or below one entry's worth — the bound that keeps an entry inside its `int16_t` slot.

### `a corrected eval never reaches the mate range`

With saturated buckets, correcting an eval that already sits just inside the mate boundary
still returns a non-mate score (`is_mate` false) in both directions — so the pruning
guards that special-case mate scores can never be fooled by a correction.

### `clear resets every bucket`

After teaching, `clear()` returns every table to zero.
