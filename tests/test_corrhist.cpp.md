# Test: `test_corrhist.cpp`

Unit tests for [correction history](../src/corrhist.hpp.md) (`src/corrhist.*`) — the
learned per-pawn-structure correction to the static eval. Strategy: drive a
`CorrectionHistory` through its public
[`update`](../src/corrhist.hpp.md#correctionhistoryupdate) API and assert the observable
contract — an empty table corrects nothing, a residual moves its bucket in its own
direction, buckets are keyed by **pawn structure** and **side to move** (and not by the
pieces), the running average saturates inside
[`CORR_LIMIT`](../src/corrhist.hpp.md#corr_limit), a corrected eval never reaches the mate
range, and `clear()` resets.

A local `teach()` helper applies the same residual repeatedly so the running average
converges toward it; the FENs are the start position, the start position with the knights
removed (identical pawns, different pieces), the same position with Black to move, and a
position with White's e-pawn advanced (a different pawn skeleton).

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

A default-constructed table reports `correction == 0` and leaves a raw eval unchanged
through [`corrected`](../src/corrhist.hpp.md#correctionhistorycorrected) — the property
that keeps a fresh search (and `bench`) starting from the raw evaluation.

### `a residual moves the bucket in its own direction`

Repeated positive residuals leave a positive correction (and `corrected(board, 0)` equals
it); repeated negative residuals leave a negative one. This is the sign convention the
search depends on: `diff = searchScore − staticEval`, so a search that keeps coming in
*above* the eval raises it.

### `correction is keyed by the pawn structure, not the pieces`

Two positions with identical pawns but different pieces share a
[`pawnStructureKey`](../src/corrhist.hpp.md#pawnstructurekey) and therefore the same
learned correction (by design — the correction is a structural prior), while a position
with an advanced pawn hashes differently and starts uncorrected.

### `correction is keyed by the side to move`

The same pawn key with the opposite side to move indexes the other table, which is still
zero after teaching only the White-to-move bucket. Scores are side-to-move relative, so
sharing one bucket across colours would fight itself.

### `the running average saturates inside CORR_LIMIT`

Hundreds of huge residuals at maximal depth leave `|correction|` at or below
`CORR_LIMIT / CORR_GRAIN` (±64 cp) in both directions — the bound that keeps an entry
inside its `int16_t` slot and stops the correction from overruling the eval.

### `a corrected eval never reaches the mate range`

With a saturated bucket, correcting an eval that already sits just inside the mate
boundary still returns a non-mate score (`is_mate` false) in both directions — so the
pruning guards that special-case mate scores can never be fooled by a correction.

### `clear resets every bucket`

After teaching a bucket, `clear()` returns it to zero.
