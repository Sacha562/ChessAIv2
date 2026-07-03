# Module: `bench`

The deterministic benchmark. Searches a fixed suite of positions to a fixed depth
and prints a `<nodes> nodes <nps> nps` summary. The node count must be **identical**
on every machine and build — it is the engine's build signature and OpenBench's
speed/consistency gate (see [PLAN.md](../PLAN.md) §29). Invoked by [uci](uci.hpp.md)
(`bench`) and [main](main.cpp.md) (`bench` subcommand).

## Source Files

- **Header (interface):** [bench.hpp](bench.hpp)
- **Source (implementation):** [bench.cpp](bench.cpp)

## How to Build & Run

```
./chessai bench          # default depth (6)
./chessai bench 8        # override depth
```

Also available as `bench [d]` inside the UCI loop. Output ends with the signature
line `<totalNodes> nodes <nps> nps`.

## Namespace

- Public API in `engine::Bench`.
- `DEFAULT_DEPTH` and the `POSITIONS[]` suite live in an anonymous namespace in
  `bench.cpp` — internal linkage.

## Functions

### `run`

Search each of the 12 built-in positions (opening / middlegame / endgame) to a fixed
depth with a fresh [`Searcher`](search.hpp.md#class-searcher) over a local
[`TranspositionTable`](tt.hpp.md#class-transpositiontable) that is **cleared before
each position**, accumulate the node counts, and print the benchmark summary. The
per-position clear keeps the total independent of position order and fully
deterministic. (Adding the TT in Phase 1a step 3 lowered the signature by design —
transpositions and TT-move ordering cut the node count.)

**When to use:** as an OpenBench signature, a quick NPS check, or a determinism
guard after changes to search/movegen.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `depth` | `int` | Search depth in plies. `depth <= 0` uses `DEFAULT_DEPTH` (6). Default argument: `0`. |

**Returns:** `void` (prints to stdout).

**Side Effects:** writes an `info string position <n> nodes <n>` line per position
and a final `<totalNodes> nodes <nps> nps` summary. Runs [`Searcher::think`](search.hpp.md#searcherthink)
with `printBest=false, printInfo=false`.

**Warnings:** the node total must stay **deterministic** — do not introduce
time-dependent or nondeterministic behavior into the searched path, or the signature
breaks. Each position uses its own local `stop` flag (never triggered here) and a
freshly-cleared TT (so a persistent table can't make the count order-dependent).
