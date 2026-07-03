# Module: `tt`

The transposition table (TT): a fixed-size cache of previously searched positions,
keyed by the board's 64-bit Zobrist hash ([`board.hash()`](../include/chess.hpp)).
It lets [search](search.hpp.md) skip re-searching a position reached by a different
move order (a *transposition*), take an early cutoff when a deep-enough result is
already known, and — most importantly at this stage — recover the best move from a
shallower search to try first. Added in **Phase 1a step 3** (see
[PLAN.md](../PLAN.md) §21).

The table is **bucketed** (4-way clusters, one cache line each), **aged** (a
per-search generation drives replacement), **lock-less** (the Hyatt XOR trick, so a
future Lazy-SMP thread can read/write without a mutex), and indexed by
**multiply-shift** (uses the full key, no power-of-two size constraint).

## Source Files

- **Header (interface):** [tt.hpp](tt.hpp)
- **Source (implementation):** [tt.cpp](tt.cpp)

## Namespace

- Public API (`Bound`, `TTProbe`, `TranspositionTable`, `valueToTT`, `valueFromTT`)
  in namespace `engine`.
- The `data` bit-layout helpers (`packData` / `unpack*` / `boundOf` / `genOf`) live
  in an anonymous namespace in `tt.cpp` — internal linkage.

## Objects / Interfaces

### `enum Bound`

How a stored `value` relates to the position's true score.

| Value | Meaning |
|-------|---------|
| `BOUND_NONE` (0) | No/empty entry. |
| `BOUND_UPPER` (1) | Fail-low: search never beat `alpha`; `value` is an **upper** bound. |
| `BOUND_LOWER` (2) | Fail-high (β-cutoff); `value` is a **lower** bound. |
| `BOUND_EXACT` (3) | PV node fully searched; `value` is exact. |

### `struct TTProbe`

The result of [`probe`](#transpositiontableprobe). `hit` is false on a miss (other
fields defaulted). `value`/`eval` are the **raw stored numbers**: a mate `value` is
relative to the stored node, so the caller must pass it through
[`valueFromTT`](#valuefromtt) before comparing to a window.

| Field | Type | Description |
|-------|------|-------------|
| `hit` | `bool` | Whether the key was found. |
| `move` | `Move` | Stored best/refutation move (`NO_MOVE` if none). |
| `value` | `Value` | Raw stored score (mate not yet rebased to the probing node). |
| `eval` | `Value` | Raw stored static eval (`VALUE_NONE` until Phase 1b uses it). |
| `depth` | `int` | Search depth the entry was stored at. |
| `bound` | `Bound` | Bound type of `value`. |

### `class TranspositionTable`

Owns the entry array. Constructed with a size in megabytes (default 16). One
instance lives in the [uci](uci.hpp.md) `Engine` and is shared by reference with
every [`Searcher`](search.hpp.md#class-searcher), so results persist across moves
(and, later, across threads); [bench](bench.hpp.md) owns its own.

| Internal type / field | Description |
|-----------------------|-------------|
| `Entry` | 16 bytes: `key` (Zobrist **XOR** `data`) + `data` (packed payload). |
| `Cluster` | `alignas(64)` bucket of `CLUSTER_SIZE` (4) entries = one cache line. |
| `data` layout | LSB→MSB: `move`(16) · `value`(16) · `eval`(16) · `depth`(8) · `genBound`(8), where `genBound` = `generation`(6) << 2 \| `bound`(2). |
| `table_` | `std::vector<Cluster>` — the entries; RAII, no raw owning pointer. |
| `generation_` | 6-bit search counter (wraps at 64); tags stores and ages entries. |

**Used by:** [search](search.hpp.md), [uci](uci.hpp.md), [bench](bench.hpp.md).

## Functions

### `valueToTT` / `valueFromTT`

Rebase a mate score for storage / retrieval. A mate is encoded root-relative
(`VALUE_MATE - ply`); the TT must store it **node-relative** so the entry is valid
at any depth it transposes to. `valueToTT` adds `ply` to a winning mate (subtracts
for a mate against), `valueFromTT` reverses it; non-mate scores and `VALUE_NONE`
pass through unchanged. See [types](types.hpp.md) for the mate encoding.

### `TranspositionTable::resize`

(Re)allocate to `mb` megabytes (`clusters = mb·2²⁰ / sizeof(Cluster)`, at least 1)
and zero the table. Called from [uci](uci.hpp.md#enginehandlesetoption) on
`setoption Hash`. **Invalidates all entries** — the caller must ensure no search is
running (the UCI handler stops the search first).

### `TranspositionTable::clear`

Zero every entry and reset the generation. Called on `ucinewgame` and before each
[bench](bench.hpp.md#run) position (for a deterministic node count).

### `TranspositionTable::newSearch`

Advance the generation (mod 64) so this search's stores out-rank older entries in
replacement. Called once by [`Searcher::think`](search.hpp.md#searcherthink).

### `TranspositionTable::probe`

Look up `key`. Scans the cluster; for each non-empty entry checks `key == entry.key
XOR entry.data` (recovers the full key and self-detects a torn write). Returns a
[`TTProbe`](#struct-ttprobe) (`hit=false` on miss). `const`; does not modify the
table.

### `TranspositionTable::store`

Insert/replace `key`'s entry with `depth`, `value` (already `valueToTT`-adjusted by
the caller), `eval`, `move`, and `bound`. Slot choice within the cluster: an empty
slot or the same key wins; otherwise the entry worth the least
(`depth − relativeAge·8`) is evicted. A same-slot store carrying `NO_MOVE`
preserves the previously stored move. Writes `key XOR data` and `data`.

### `TranspositionTable::prefetch`

Issue a `_mm_prefetch` for `key`'s cluster (no-op on non-GCC/Clang). Called by
[search](search.hpp.md) via `board.zobristAfter(move)` before `makeMove`, hiding TT
memory latency behind the make/recurse work.

### `TranspositionTable::hashfull`

Return per-mille fill for `info hashfull`: of the first 1000 entry slots, the
fraction that are occupied at the **current** generation. `const`.

## Related Test Documentation

- [test_tt.cpp.md](../tests/test_tt.cpp.md) — round-trip, mate-rebasing, replacement
  / aging, zero-key, resize/clear, and randomized-stress unit tests for this module.

## Related

- Consumer & integration rules (cutoff/PV/mate care): [search](search.hpp.md).
- Score & mate conventions: [types](types.hpp.md).
- Sizing option & lifetime: [uci](uci.hpp.md).
- Design rationale (replacement, lock-less, indexing): [PLAN.md](../PLAN.md) §21–22.
