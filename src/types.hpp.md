# Module: `types`

Core engine types and score conventions shared across the engine. Header-only:
it defines the centipawn score type, the mate-encoding constants, and small
`constexpr` helpers for constructing and testing mate scores. Every module that
produces or interprets a search score depends on this file — chiefly
[eval](eval.hpp.md) (produces scores) and [search](search.hpp.md) (compares and
propagates them under negamax).

Scores are centipawns from the **side-to-move's** perspective (negamax). Mate is
encoded as `VALUE_MATE - ply`, so a shorter mate scores higher and mate scores
survive being stored/retrieved in the transposition table (arriving in Phase 1a).

## Source Files

- **Header (interface):** [types.hpp](types.hpp)

## Namespace

- All symbols are declared in namespace `engine`.
- Re-exports two library types into `engine` for convenience:
  `using chess::Board;` and `using chess::Move;`.

## Objects / Interfaces

### `using Value = int`

The score type: a signed centipawn value from the side-to-move's perspective.
Positive = better for the side to move; `0` = equal/draw.

### Score & search constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `MAX_PLY` | `128` | Maximum search depth in plies. |
| `VALUE_ZERO` | `0` | Neutral score. |
| `VALUE_DRAW` | `0` | Draw score (repetition, 50-move, insufficient material, stalemate). |
| `VALUE_MATE` | `32000` | Mate delivered at the root (ply 0). |
| `VALUE_INFINITE` | `32001` | Alpha-beta window bound (`±∞`). |
| `VALUE_NONE` | `32002` | Sentinel for "no score" (e.g. an uninitialized TT/eval slot). |
| `VALUE_MATE_IN_MAX_PLY` | `VALUE_MATE - MAX_PLY` | Threshold above which a score is a mate for the side to move. |
| `VALUE_MATED_IN_MAX_PLY` | `-VALUE_MATE_IN_MAX_PLY` | Threshold below which a score is a mate against the side to move. |

**Used by:** [eval](eval.hpp.md#evaluate), [search](search.hpp.md)

### `Board`, `Move` (re-exported)

Aliases for `chess::Board` and `chess::Move` from the vendored library, so engine
code can write `Board` / `Move` unqualified. The library is authoritative for their
behavior (see `include/chess.hpp`, third-party — not documented here).

## Functions

### `mate_in`

Score for delivering mate `ply` plies from the root. Returns `Value` = `VALUE_MATE - ply`. `constexpr`.

### `mated_in`

Score for being mated `ply` plies from the root. Returns `Value` = `-VALUE_MATE + ply`. `constexpr`.

### `is_mate`

Return `true` if `v` is a mate score for either side (i.e. `v >= VALUE_MATE_IN_MAX_PLY`
or `v <= VALUE_MATED_IN_MAX_PLY`). `constexpr`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `v` | `Value` | Score to test. |

**Returns:** `bool` — whether `v` encodes a forced mate.

**Used by:** [search](search.hpp.md) (mate-cutoff in iterative deepening, and `scoreToUci` mate formatting).
