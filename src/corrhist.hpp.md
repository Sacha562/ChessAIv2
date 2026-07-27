# Module: `corrhist`

**Correction history** (Phase 1d): a learned, per-pawn-structure correction to the static
evaluation.

[`evaluate`](eval.hpp.md#evaluate) is a static guess; the **search** is the ground truth.
Every time a node finishes, the gap between the two — `searchScore − staticEval` — is a
residual the hand-crafted eval got wrong *in that kind of position*. `CorrectionHistory`
remembers that residual as a depth-weighted running average, bucketed by the **pawn
skeleton** and the side to move, and [`search`](search.hpp.md#searchersearch) adds it back
the next time a similar position is evaluated. Everything downstream of the static eval —
reverse futility pruning, futility pruning, null-move pruning, razoring, the `improving`
trend, and the q-search stand-pat — then reasons about an eval the search has already
been observed to disagree with, without any of them changing.

Bucketing by pawn structure is what makes this cheap and effective: pawns move rarely, so
the same bucket is hit constantly inside one search, and the pawn skeleton is exactly what
a hand-crafted eval judges worst (long-term structural compensation that only shows up
several plies deep). The bucket key is a **multiply-mix of the two pawn bitboards**
([`pawnStructureKey`](#pawnstructurekey)), not an incrementally-maintained Zobrist key —
a couple of ALU ops on the per-node path, and a collision merely blurs two buckets.

**Hot path:** yes — [`correction`](#correctionhistorycorrection) runs once per non-check
node in [`search`](search.hpp.md#searchersearch) and once per q-search stand-pat;
[`update`](#correctionhistoryupdate) runs at most once per completed main-search node. No
allocation, no locks: one `CorrectionHistory` is owned **per
[`Searcher`](search.hpp.md#class-searcher)** (64 KB inline), so it is single-thread state
today and per-thread state under Lazy SMP later.

## Source Files

- **Header (interface):** [corrhist.hpp](corrhist.hpp)
- **Source (implementation):** [corrhist.cpp](corrhist.cpp)

## Related Test Documentation

- [test_corrhist.cpp.md](../tests/test_corrhist.cpp.md)

## Namespace

- Public API (`CORR_GRAIN`, `CORR_LIMIT`, `pawnStructureKey`, `class CorrectionHistory`)
  in namespace `engine`.
- The mixing constants `MIX_WHITE` / `MIX_BLACK` live in an anonymous namespace in
  `corrhist.cpp` — internal linkage, **not** public API.

## Objects / Interfaces

### `CORR_GRAIN`

`constexpr int CORR_GRAIN = 256`. Fixed-point scale of a stored entry: an entry holds
`centipawns · CORR_GRAIN`, so the running average keeps sub-centipawn resolution inside
its `int16_t` slot and only rounds to whole centipawns when the correction is applied.

### `CORR_LIMIT`

`constexpr int CORR_LIMIT = 64 · CORR_GRAIN`. Saturating bound on a stored entry, and
therefore the largest correction ever applied to a static eval: **±64 cp**. Deliberately
small — the correction nudges the eval, it never overrules it — and it keeps every entry
well inside `int16_t`.

### `class CorrectionHistory`

Per-(side to move, pawn structure) running average of the search-vs-static-eval residual.
Default-constructed empty (all buckets zero → no correction), so a fresh search starts
from the raw eval and a `bench` run stays deterministic.

| Member | Type | Description |
|--------|------|-------------|
| `table_` | `int16_t[2][SIZE]` | The buckets: `[side to move][pawn-key index]`, each bounded to `±CORR_LIMIT`. `SIZE` = 16384 (power of two) → 64 KB total. |
| `SIZE` | `static constexpr int` | Buckets per colour; a power of two so indexing is a mask. |
| `MAX_WEIGHT` | `static constexpr int` | Depth at which a residual carries full weight (16). |
| `WEIGHT_DIV` | `static constexpr int` | Running-average denominator (`2 · MAX_WEIGHT`): a full-weight update moves an entry half-way to the new residual. |

**Methods:** [`clear`](#correctionhistoryclear),
[`correction`](#correctionhistorycorrection), [`corrected`](#correctionhistorycorrected),
[`update`](#correctionhistoryupdate) (public); `index` (private).

**Used by:** [`Searcher`](search.hpp.md#class-searcher) (owns one as `corrHist_`, reached
through [`Searcher::correctedEval`](search.hpp.md#searchercorrectedeval)).

## Functions

### `pawnStructureKey`

Hash of the pawn skeleton: both colours' pawn bitboards multiplied by distinct odd
constants, XOR-mixed, avalanched (`key ^= key >> 29`) and multiplied once more so the
high bits are well distributed.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position whose pawn structure is hashed. Read-only. |

**Returns:** `uint64_t` — the bucket key. Equal pawn placements always hash equally; it is
**not** a Zobrist key and is never maintained incrementally.

**Performance:** two bitboard reads, two multiplies, a shift and an XOR — cheap enough to
recompute at every node rather than thread a key through the search.

**Warnings:** only pawns enter the key. Two positions with identical pawns but different
pieces share a bucket **by design** (the correction is a structural prior), and unrelated
structures can collide in the 16384-bucket index — both merely average two residuals
together, which is why the entry is bounded by [`CORR_LIMIT`](#corr_limit).

### `CorrectionHistory::clear`

Reset every bucket to "no correction known" (all-zero). Called by the constructor;
available for reuse/tests.

### `CorrectionHistory::correction`

The correction learned for `board`'s pawn structure and side to move.

**Parameters:** `board` (`const Board&`).
**Returns:** `Value` (centipawns) in `[−CORR_LIMIT / CORR_GRAIN, +CORR_LIMIT /
CORR_GRAIN]` = `[−64, +64]`; `0` for an untouched bucket. Side-to-move relative, matching
[`evaluate`](eval.hpp.md#evaluate)'s convention.

**Performance:** one [`pawnStructureKey`](#pawnstructurekey) mix plus one array read.

### `CorrectionHistory::corrected`

`raw` plus [`correction`](#correctionhistorycorrection), clamped into
`(VALUE_MATED_IN_MAX_PLY, VALUE_MATE_IN_MAX_PLY)` so a corrected static eval can never be
mistaken for a mate score by the pruning guards that test `is_mate`.

**Parameters:** `board` (`const Board&`), `raw` (`Value`, a side-to-move-relative static
eval).
**Returns:** `Value` — the eval the search should reason about.

### `CorrectionHistory::update`

Fold one node's residual into its bucket as a depth-weighted running average:

```
weight  = min(depth + 1, MAX_WEIGHT)
scaled  = clamp(diff · CORR_GRAIN, ±CORR_LIMIT)
entry   = clamp((entry · (WEIGHT_DIV − weight) + scaled · weight) / WEIGHT_DIV, ±CORR_LIMIT)
```

A deeper node's disagreement is more trustworthy, so it moves the entry further; the
clamps keep the entry inside `int16_t` and cap the applied correction at ±64 cp.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | The node's position (keys the bucket). |
| `depth` | `int` | Remaining depth of the node that produced `diff`; weights the update. |
| `diff` | `Value` | Residual `searchScore − staticEval` in cp, measured against the **corrected** eval the node actually used — so the average converges on zero residual rather than drifting. |

**Side Effects:** writes one `int16_t` bucket.

**Warnings:** the caller
([`Searcher::search`](search.hpp.md#searchersearch)) decides *when* a residual is
trustworthy, and those guards are part of the contract: not in check, a real **quiet**
best move (a capture's score reflects material, not structure), no mate scores, not inside
a singular-verification search, and the bound must not contradict the residual's sign (a
`BOUND_LOWER` node may only teach that the eval was too **low**, a `BOUND_UPPER` node only
that it was too **high**).
