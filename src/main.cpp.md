# Module: `main`

The program entry point and command dispatch. A source-only file (no header): it
parses `argv` and routes to one of three modes — the [uci](uci.hpp.md) loop
(default), the [bench](bench.hpp.md) benchmark, or the [perft](perft.hpp.md)
correctness suite.

## Source Files

- **Source:** [main.cpp](main.cpp)

## How to Build & Run

Build per [build.md](../build.md) / [README.md](../README.md), then:

```
./chessai                -> UCI loop (default; for a GUI / cutechess / fastchess)
./chessai bench [depth]  -> deterministic benchmark (OpenBench signature)
./chessai perft test     -> perft correctness suite (exit 1 on failure)
```

## Namespace

- `main` is at global scope (C++ requires it). It calls into namespace `engine`.

## Functions

### `main`

Parse the first CLI argument and dispatch.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `argc` | `int` | Argument count. |
| `argv` | `char**` | Argument vector. `argv[1]` selects the mode; `argv[2]` is an optional depth (`bench`) or the literal `test` (`perft`). |

**Returns:** `int` — process exit code. `0` on success; `1` when `perft test`
reports a mismatch (via [`Perft::test`](perft.hpp.md#test)).

**Behavior / dispatch:**
- `bench` → [`Bench::run`](bench.hpp.md#run) with optional depth (`std::atoi(argv[2])`, `0` if absent).
- `perft test` (exactly) → [`Perft::test`](perft.hpp.md#test); exit `0`/`1` on pass/fail.
- anything else (including no args) → [`run_uci`](uci.hpp.md#run_uci).

**Side Effects:** delegates all I/O to the selected mode.
