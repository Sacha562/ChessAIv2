# Test: `test_nnue.cpp`

Unit tests for [NNUE inference](../src/nnue.hpp.md) (`src/nnue.*`). They are **property
checks** against a synthetic network filled with deterministic random weights
(`randomNetwork(seed)`: weights in `±40`, mostly positive feature biases so SCReLU is live),
so the suite needs no trained net file and catches indexing and delta bugs regardless of
what any real network learned.

Agreement with a *trained* network was checked separately: net1's engine evals
(`33 / 1378 / −1311 / 43` cp) against the trainer's float evals (`31.0 / 1380.2 / −1316.1 /
33.6`). See [nnue.hpp.md](../src/nnue.hpp.md#the-network).

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
position, for both perspectives and all 512 lanes. Over 1000 moves must be checked.

### `a colour-mirrored position evaluates the same for the side to move`

A position and its colour-flipped twin (ranks mirrored, colours swapped, the other side to
move) must give the identical [`evaluate`](../src/nnue.hpp.md#evaluate) score. Run after
1.e4 and on an Italian-opening middlegame. This pins the perspective mirroring
(`sq ^ 56`) and the own/theirs feature split.

### `loadNetwork rejects a missing or short file`

[`loadNetwork`](../src/nnue.hpp.md#loadnetwork) returns `nullptr` for a nonexistent path
and for a 9-byte file (written to the working directory and removed afterwards).
