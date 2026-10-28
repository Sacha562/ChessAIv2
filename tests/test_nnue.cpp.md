# Test: `test_nnue.cpp`

Unit tests for [NNUE inference](../src/nnue.hpp.md) (`src/nnue.*`). They are **property
checks** against a synthetic network filled with deterministic random weights
(`randomNetwork(seed)`: weights in `±40`, mostly positive feature biases so SCReLU is live),
so the suite needs no trained net file and catches indexing and delta bugs regardless of
what any real network learned.

Agreement with a *trained* network was checked separately: net2's engine evals
(`24 / 1470 / −1404 / 35 / 485` cp) against the trainer's float evals (`33.7 / 1463.6 /
−1387.8 / 42.5 / 488.2`). See [nnue.hpp.md](../src/nnue.hpp.md#the-network).

## Test Code

- **Test Code:** [test_nnue.cpp](test_nnue.cpp)
- **Module under test:** [nnue.hpp.md](../src/nnue.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `incremental updates match a full refresh through random playouts`

From five start positions chosen to reach castling (both sides, both wings), en passant,
and promotions (with and without capture) early, eight random playouts of up to 120 plies
each. After every move, the accumulator built by [`update`](../src/nnue.hpp.md#update) from
the parent must equal a from-scratch [`refresh`](../src/nnue.hpp.md#refresh) of the new
position, for both perspectives and all 1024 lanes. Over 1000 moves must be checked.

### `a colour-mirrored position evaluates the same for the side to move`

A position and its colour-flipped twin (ranks mirrored, colours swapped, the other side to
move) must give the identical [`evaluate`](../src/nnue.hpp.md#evaluate) score. Run after
1.e4 and on an Italian-opening middlegame. This pins the perspective mirroring
(`sq ^ 56`) and the own/theirs feature split.

### `the output bucket follows the piece count`

With every output weight zeroed and bucket `b`'s bias set to give exactly `10 · (b + 1)` cp,
[`evaluate`](../src/nnue.hpp.md#evaluate) returns the bias of the bucket the position falls
in, so the score names the bucket. Positions with 3, 5, 6, 29, 30, 31 and 32 pieces must
land in buckets 0, 0, 1, 6, 7, 7 and 7. This pins the `(pieces − 2) / 4` rule at both ends
and across bucket edges, which must match the trainer's `MaterialCount<8>`.

### `the embedded default network loads and judges material sensibly`

When the test build embeds the default network (the Makefile and CMake pass `EVALFILE`),
[`embeddedNetwork`](../src/nnue.hpp.md#embeddednetwork) returns the same non-null pointer on
every call. The network scores the start position within ±100 cp, and a position missing
Black's queen (White to move) above +800 cp, and one missing White's queen (White to move)
below −800 cp. The bounds are loose on purpose, so retraining the default net doesn't break the
test. A build without an embedded network skips the checks.

### `loadNetwork rejects a missing or short file`

[`loadNetwork`](../src/nnue.hpp.md#loadnetwork) returns `nullptr` for a nonexistent path
and for a 9-byte file (written to the working directory and removed afterwards).
