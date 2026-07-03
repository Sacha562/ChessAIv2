# Test: `test_tt.cpp`

Unit tests for the [transposition table](../src/tt.hpp.md) (`src/tt.*`). They
exercise the **public API only** — `store` / `probe` / `resize` / `clear` /
`newSearch` / `hashfull` / `prefetch` and the `valueToTT` / `valueFromTT` helpers —
with a mix of **known-answer** checks (a stored entry probes back exactly),
**property** checks (round-trip identities, determinism), and a **randomized stress**
pass. Replacement and aging are made deterministic by using a single-cluster table
(`TranspositionTable(0)`, whose multiply-shift index maps every key to cluster 0),
so eviction order is fully controlled.

## Test Code

- **Test Code:** [test_tt.cpp](test_tt.cpp)
- **Module under test:** [tt.hpp.md](../src/tt.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `valueToTT / valueFromTT rebase mates and pass everything else through`

Non-mate scores and `VALUE_NONE` are returned unchanged at every ply; a mate
`ply + j` from the root is stored node-relative as `mate_in(j)` / `mated_in(j)` and
`valueFromTT(valueToTT(v)) == v` for both winning and losing mates.

### `probe on an empty table misses`

A freshly sized table returns `hit == false` for any key (including `0`).

### `store then probe round-trips every packed field`

`depth`, `value`, `eval`, `move`, and `bound` all come back exactly as stored.

### `re-storing the same key updates it in place`

A second store for the same key replaces depth/value/move/bound.

### `a same-slot store carrying NO_MOVE preserves the stored move`

Storing with `NO_MOVE` on an existing key keeps the previously stored move while
updating the other fields (mirrors a fail-low re-store that has no best move).

### `every Bound flag round-trips`

`BOUND_UPPER` / `BOUND_LOWER` / `BOUND_EXACT` survive pack/unpack.

### `extreme values and evals survive the int16 pack`

Boundary numbers spanning the full `int16` range (`±VALUE_MATE`, `VALUE_NONE`,
`32767`, `-32768`, …) round-trip through the 16-bit value/eval fields.

### `depth round-trips across the signed int8 range`

Depths from `-128` to `127` (negatives reserved for future qsearch entries) survive
the 8-bit depth field.

### `a mate score survives storage node-relative and rebases on read`

An end-to-end check: store `valueToTT(mate, ply)`, then confirm the raw entry holds
the node-relative mate and `valueFromTT` restores the root-relative score.

### `a zero Zobrist key is stored and probed correctly`

Key `0` (which an empty slot's `key ^ data` also yields) is stored and found via the
`data != 0` guard, and other keys still miss against it.

### `bucket replacement evicts the shallowest entry first`

With a full 4-way cluster, a new store evicts the lowest-depth entry.

### `older generations are replaced before fresh ones of equal depth`

After `newSearch`, entries from a prior generation are evicted before same-depth
entries stored in the current generation.

### `clear and resize drop all entries`

Both `clear()` and `resize()` leave every prior key missing.

### `hashfull reports current-generation occupancy in permille`

Empty → `0`; two of four slots at the current generation → `500`.

### `prefetch on any key is safe`

`prefetch` on arbitrary keys does not crash.

### `randomized stress: immediate probe always hits and round-trips`

20,000 xorshift keys are each stored then immediately probed; every probe hits and
returns the stored value/move (exercises index math and packing without eviction).

### `identical operation sequences produce identical results`

Two independent tables driven by the same op sequence yield identical hit counts
(determinism guard).
