# Module: `main`

The program entry point and command dispatch. A source-only file (no header): it
parses `argv` and routes to one of four modes — the [uci](uci.hpp.md) loop
(default), the [bench](bench.hpp.md) benchmark, the [perft](perft.hpp.md)
correctness suite, or [datagen](datagen.hpp.md) self-play training-data generation.

## Source Files

- **Source:** [main.cpp](main.cpp)

## How to Build & Run

Build per [build.md](../build.md) / [README.md](../README.md), then:

```
./chessai                -> UCI loop (default; for a GUI / cutechess / fastchess)
./chessai bench [depth]  -> deterministic benchmark (OpenBench signature)
./chessai perft test     -> perft correctness suite (exit 1 on failure)
./chessai datagen <out-prefix> <threads> <positions> [seed] [soft-nodes]
                         -> self-play NNUE training data, viriformat (exit 1 on bad args)
```

## Namespace

- `main` is at global scope (C++ requires it). It calls into namespace `engine`.
- `DATAGEN_MIN_ARGS`, `randomSeed`, and `parseDatagen` live in an anonymous namespace in
  `main.cpp` — internal linkage.

## Functions

### `randomSeed` (internal)

Return `uint64_t` — 64 bits from two `std::random_device` draws: the default datagen seed,
so that two runs appending to the same files never replay the same games.
[`runDatagen`](datagen.hpp.md#rundatagen) prints the seed, so a run can still be repeated.

### `parseDatagen` (internal)

Build a [`DatagenConfig`](datagen.hpp.md#struct-datagenconfig) from
`chessai datagen <out-prefix> <threads> <positions> [seed] [soft-nodes]`.

**Parameters:** `argc` (`int`), `argv` (`char**`) — the full command line; `argv[1]` is
`datagen`.

**Returns:** `std::optional<engine::DatagenConfig>` — the config, or `std::nullopt` when
fewer than three arguments follow `datagen` or a value is out of range (`threads < 1`,
`positions == 0`, `soft-nodes < 1`). Numbers are read with `atoi` / `strtoull` /
`strtoll`, so a non-numeric value reads as `0` and is rejected by those checks. `seed`
defaults to [`randomSeed`](#randomseed-internal) and `soft-nodes` to 5000.

### `main`

Parse the first CLI argument and dispatch.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `argc` | `int` | Argument count. |
| `argv` | `char**` | Argument vector. `argv[1]` selects the mode; `argv[2]` is an optional depth (`bench`), the literal `test` (`perft`), or the first datagen argument. |

**Returns:** `int` — process exit code. `0` on success; `1` when `perft test`
reports a mismatch (via [`Perft::test`](perft.hpp.md#test)), when the `datagen`
arguments are invalid, or when [`runDatagen`](datagen.hpp.md#rundatagen) cannot open an
output file.

**Behavior / dispatch:**
- `bench` → [`Bench::run`](bench.hpp.md#run) with optional depth (`std::atoi(argv[2])`, `0` if absent).
- `perft test` (exactly) → [`Perft::test`](perft.hpp.md#test); exit `0`/`1` on pass/fail.
- `datagen` → [`parseDatagen`](#parsedatagen-internal), then
  [`runDatagen`](datagen.hpp.md#rundatagen); on invalid arguments a usage line goes to
  stderr and the exit code is `1`.
- anything else (including no args) → [`run_uci`](uci.hpp.md#run_uci).

**Side Effects:** delegates all I/O to the selected mode.
