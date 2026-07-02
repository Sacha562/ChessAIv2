# Module: `perft`

Move-generation correctness and speed measurement. `perft` (performance test) counts
leaf nodes of the game tree to a fixed depth; comparing the count against known-good
values validates make/unmake and staged-generation glue. Since the vendored library
is already perft-correct, this mainly guards **our** integration. Invoked by
[uci](uci.hpp.md) (`perft` / `perft test`) and [main](main.cpp.md) (`perft test`
subcommand).

Per [PLAN.md](../PLAN.md) §29, perft must be green before any SPRT time is spent —
testing a buggy move generator wastes cores and misleads.

## Source Files

- **Header (interface):** [perft.hpp](perft.hpp)
- **Source (implementation):** [perft.cpp](perft.cpp)

## How to Build & Run

```
./chessai perft test          # run the correctness suite; exit 1 on any mismatch
# or, interactively in UCI mode:
perft 5                       # divide (per-root-move breakdown) from the current position
perft test                    # run the suite
```

## Namespace

- All functions in `engine::Perft`.
- `uci::moveToUci` used in `divide` is a **library** function (`chess::uci::…`).

## Functions

### `perft`

Count leaf nodes at the given depth from `board`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `chess::Board&` | Position to count from; mutated via make/unmake and restored (in/out). |
| `depth` | `int` | Plies to descend. `depth <= 0` counts as 1 node; `depth == 1` returns the legal-move count directly (bulk counting). |

**Returns:** `uint64_t` — number of leaf nodes at `depth`.

**Side Effects:** transiently mutates `board`; no I/O.

**Performance:** ~350–500M nps on this machine (library-provided speed). Uses the
`depth == 1` bulk-count shortcut (no make/unmake on the last ply).

### `divide`

Perft with a per-root-move breakdown (like Stockfish's `go perft`): prints each root
move with its subtree node count, then the total, elapsed time, and NPS.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `chess::Board&` | Position to divide from (in/out, restored). |
| `depth` | `int` | Plies. `depth <= 0` prints `Nodes: 1`. |

**Returns:** `void` (prints to stdout).

### `test`

Run a fixed suite of six standard positions (startpos d6 = 119,060,324, Kiwipete,
and CPW positions 3–6) and compare each count against its known value.

**Parameters:** none.

**Returns:** `bool` — `true` iff **every** position matches. Prints an `[ OK ]` /
`[FAIL]` line per case and a final summary. [main](main.cpp.md) maps `false` to exit
code 1.
