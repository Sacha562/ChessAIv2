# Module: `corrhist`

**Correction history** (Phase 1d, widened after v0.6): a learned correction to the static
evaluation, read from five tables at once.

The static eval ([NNUE](nnue.hpp.md) or [`evaluate`](eval.hpp.md#evaluate)) is a guess;
the **search** is the ground truth. Every time a node finishes, the gap between the two —
`searchScore − correctedEval` — is a residual the eval still gets wrong *in that kind of
position*. `CorrectionHistory` remembers that residual in several tables, each bucketing
the position by a different **view**, and [`search`](search.hpp.md#searchersearch) adds
their weighted sum back the next time a similar position is evaluated. Everything
downstream of the static eval — reverse futility pruning, futility pruning, null-move
pruning, razoring, the `improving` trend, and the q-search stand-pat — then reasons about
an eval the search has already corrected, without any of them changing.

| Table | Keyed by | What it catches |
|-------|----------|-----------------|
| pawn | both sides' pawns ([`pawnStructureKey`](#pawnstructurekey)) | Long-term structural compensation; pawns move rarely, so a bucket is revisited constantly. The original (v0.1d) table. |
| non-pawn ×2 | one key per colour over that side's knights, bishops, rooks, queens and king ([`nonPawnKey`](#nonpawnkey)) — two lookups per node | Each side's piece placement, independently of the pawns. |
| minor | knights, bishops and kings of both sides ([`minorKey`](#minorkey)) | Minor-piece configurations (outposts, bad bishops) against king placement. |
| major | rooks, queens and kings of both sides ([`majorKey`](#majorkey)) | Heavy-piece configurations (open files, queen activity) against king placement. |
| continuation | the previous move's (piece, to-square) | The threat or tension the last move created, which a static eval reads poorly. |

Every table is split by **side to move** (scores are side-to-move relative). Keys are
**multiply-mixes of a few bitboards**, not incrementally maintained Zobrist keys — a few
ALU ops per table on the per-node path, and a collision merely blurs two buckets.

**Learning rule.** Every entry learns with the **gravity** update the other history
tables use (`entry += bonus − entry·|bonus| / CORR_LIMIT`), driven by the residual against
the **corrected** eval the node actually used. The tables therefore cooperate: their
weighted sum converges on the true residual, instead of each table separately claiming all
of it (which a plain sum of independent running averages would do). Before the widening,
the pawn table was a running average toward the corrected-eval residual, whose fixed point
is only about **half** the true residual.

**Hot path:** yes — [`slots`](#correctionhistoryslots) and
[`correction`](#correctionhistorycorrection) run once per non-check node in
[`search`](search.hpp.md#searchersearch) and once per q-search stand-pat;
[`update`](#correctionhistoryupdate) runs at most once per completed main-search node. No
allocation on that path, no locks: one `CorrectionHistory` is owned **per
[`Searcher`](search.hpp.md#class-searcher)**, its ~330 KB of tables heap-allocated once at
construction (they would crowd the search thread's stack), so it is single-thread state
today and per-thread state under Lazy SMP later.

## Source Files

- **Header (interface):** [corrhist.hpp](corrhist.hpp)
- **Source (implementation):** [corrhist.cpp](corrhist.cpp)

## Related Test Documentation

- [test_corrhist.cpp.md](../tests/test_corrhist.cpp.md)

## Namespace

- Public API (`CORR_GRAIN`, `CORR_LIMIT`, `struct CorrWeights`, `struct CorrSlots`,
  `pawnStructureKey`, `nonPawnKey`, `minorKey`, `majorKey`, `class CorrectionHistory`) in
  namespace `engine`.
- Internal helpers live in an anonymous namespace in `corrhist.cpp` — internal linkage,
  **not** public API: the mixing constants `MIX[colour][piece type]` and `FINAL_MIX`, the
  piece-set arrays `PAWNS` / `NON_PAWNS` / `MINORS` / `MAJORS`, `sideMix` (one colour's
  mix over a piece set), `finish` (the avalanche step), `bothSidesKey` (both colours'
  mixes, avalanched), `gravity` (the entry update) and `PERCENT`.

## Objects / Interfaces

### `CORR_GRAIN`

`constexpr int CORR_GRAIN = 256`. Fixed-point scale of a stored entry: an entry holds
`centipawns · CORR_GRAIN`, so the learned value keeps sub-centipawn resolution inside its
`int16_t` slot and only rounds to whole centipawns when the correction is read.

### `CORR_LIMIT`

`constexpr int CORR_LIMIT = 64 · CORR_GRAIN`. Saturating bound on **one** stored entry:
**±64 cp**. Deliberately small — each table nudges the eval, none overrules it — and it
keeps every entry well inside `int16_t`. The summed correction can exceed it (up to six
entries at full weight); [`corrected`](#correctionhistorycorrected) still keeps the result
out of the mate range.

### `struct CorrWeights`

Per-table read weights, in **percent**: a table's entry counts `weight / 100` of its
value. `0` switches a table's read off (it keeps learning, but never contributes). Filled
from the `corr*Weight` [`Tunables`](search.hpp.md#struct-tunables) at the start of each
search, and so UCI/SPSA-tunable (`CorrPawnWeight` … `CorrContWeight`, see
[uci.hpp.md](uci.hpp.md)).

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `pawn` | `int` | `100` | Weight of the pawn-structure table. |
| `nonPawn` | `int` | `100` | Weight of **each** colour's non-pawn lookup (two entries per read). |
| `minor` | `int` | `100` | Weight of the minor-piece table. |
| `major` | `int` | `100` | Weight of the major-piece table. |
| `cont` | `int` | `100` | Weight of the continuation table (read only when a previous move exists). |

**Used by:** [`CorrectionHistory::setWeights`](#correctionhistorysetweights),
[`Searcher::think`](search.hpp.md#searcherthink).

### `struct CorrSlots`

The bucket a position reads and writes in every table, hashed **once per node** by
[`slots`](#correctionhistoryslots) so the read at the static eval and the write after the
move loop are guaranteed to agree (the board is restored by then, but this also saves
re-hashing).

| Field | Type | Description |
|-------|------|-------------|
| `stm` | `int` | Side to move (`0` = white) — selects each table's half. |
| `pawn` | `int` | Index into the pawn table. |
| `nonPawn` | `int[2]` | Index into the non-pawn table for white's (`[0]`) and black's (`[1]`) key. |
| `minor` | `int` | Index into the minor table. |
| `major` | `int` | Index into the major table. |
| `contPiece` | `int` | The previous move's piece (colour+type, 0..11), or `-1` when there is none (root, after a null move) — then the continuation table is neither read nor written. |
| `contTo` | `int` | The previous move's to-square (0..63). |

**Used by:** [`Searcher::corrSlots`](search.hpp.md#searchercorrslots),
[`Searcher::correctedEval`](search.hpp.md#searchercorrectedeval), and every
`CorrectionHistory` read/write method.

### `class CorrectionHistory`

Per-side-to-move learned residuals of the static eval over the five views above.
Constructed empty (all buckets zero → no correction), so a fresh search starts from the
raw eval and a `bench` run stays deterministic.

| Member | Type | Description |
|--------|------|-------------|
| `tables_` | `std::unique_ptr<Tables>` | The tables, heap-owned (~330 KB). `Tables` holds `pawn[2][SIZE]`, `nonPawn[2][2][SIZE]` (`[side to move][colour keyed][bucket]`), `minor[2][SIZE]`, `major[2][SIZE]` and `cont[2][12][64]`, all `int16_t` bounded to `±CORR_LIMIT`. |
| `weights_` | [`CorrWeights`](#struct-corrweights) | Read weights for the current search. |
| `SIZE` | `static constexpr int` | Buckets per side to move in each keyed table (16384, a power of two, so indexing is a mask). |
| `MAX_WEIGHT` | `static constexpr int` | Depth at which a residual carries full weight (16). |
| `BONUS_DIV` | `static constexpr int` | Step divisor: one update moves an entry by `diff · CORR_GRAIN · min(depth + 1, MAX_WEIGHT) / 128` (≈ 1/16 of the residual at depth 7, per table). |
| `MAX_BONUS` | `static constexpr int` | Largest single step, `CORR_LIMIT / 4` (16 cp), so one wild residual cannot slam an entry to its limit. |

**Methods:** [`clear`](#correctionhistoryclear),
[`setWeights`](#correctionhistorysetweights), [`slots`](#correctionhistoryslots),
[`correction`](#correctionhistorycorrection), [`corrected`](#correctionhistorycorrected),
[`update`](#correctionhistoryupdate) (public); `bucket` (private: the top 32 key bits
masked to `SIZE`).

**Used by:** [`Searcher`](search.hpp.md#class-searcher) (owns one as `corrHist_`, reached
through [`Searcher::correctedEval`](search.hpp.md#searchercorrectedeval)).

## Functions

### `pawnStructureKey`

Hash of the pawn skeleton: both colours' pawn bitboards multiplied by distinct odd
constants, XOR-mixed, avalanched (`key ^= key >> 29`) and multiplied once more by
`FINAL_MIX` so the high bits are well distributed. Bit-for-bit the key of the original
pawn-only table.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position whose pawn structure is hashed. Read-only. |

**Returns:** `uint64_t` — the bucket key. Equal pawn placements always hash equally; it is
**not** a Zobrist key and is never maintained incrementally.

**Performance:** two bitboard reads, two multiplies, a shift and an XOR.

**Warnings:** only pawns enter the key. Two positions with identical pawns but different
pieces share a bucket **by design** (the correction is a structural prior), and unrelated
structures can collide in the 16384-bucket index — both merely blend two residuals, which
is why each entry is bounded by [`CORR_LIMIT`](#corr_limit).

### `nonPawnKey`

Hash of one colour's knights, bishops, rooks, queens and king (each piece type mixed with
its own `MIX[colour][type]` constant, so a knight and a bishop on the same square hash
differently), avalanched like the pawn key.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position to hash. Read-only. |
| `color` | `Color` | Whose pieces enter the key. |

**Returns:** `uint64_t` — the bucket key; unchanged by any pawn move and by the other
colour's moves.

### `minorKey`

Hash of the knights, bishops and kings of **both** colours. The king is included so a
minor-piece configuration is learned relative to where the kings stand.

**Parameters:** `board` (`const Board&`, read-only).
**Returns:** `uint64_t` — the bucket key; unchanged by pawn, rook and queen moves.

### `majorKey`

Hash of the rooks, queens and kings of **both** colours.

**Parameters:** `board` (`const Board&`, read-only).
**Returns:** `uint64_t` — the bucket key; unchanged by pawn, knight and bishop moves.

### `CorrectionHistory::clear`

Reset every bucket of every table to "no correction known" (all-zero). Called by the
constructor; available for reuse/tests. Weights are left as they are.

### `CorrectionHistory::setWeights`

Install the [`CorrWeights`](#struct-corrweights) used by every later read. Called once per
search by [`Searcher::think`](search.hpp.md#searcherthink) from the `corr*Weight`
[`Tunables`](search.hpp.md#struct-tunables).

### `CorrectionHistory::slots`

Hash `board` into its bucket in every table (`static`: it depends only on the position and
the previous move).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | The node's position. Read-only. |
| `prevPiece` | `int` | Piece (colour+type, 0..11) of the move that led here, or `-1` for none. |
| `prevTo` | `int` | To-square (0..63) of that move; ignored when `prevPiece` is `-1`. |

**Returns:** [`CorrSlots`](#struct-corrslots) — every field filled.

**Performance:** five key computations (one pawn, two non-pawn, minor, major): roughly
two dozen bitboard reads and multiplies — small beside an NNUE evaluation.

### `CorrectionHistory::correction`

The weighted sum of every table's entry for `s`:
`(Σ weight · entry) / (100 · CORR_GRAIN)`, the continuation entry included only when
`s.contPiece >= 0`.

**Parameters:** `s` (`const CorrSlots&`).
**Returns:** `Value` (centipawns), side-to-move relative, matching the eval's convention;
`0` for untouched buckets. At default weights at most six entries of ±64 cp each.

**Performance:** five or six array reads and multiplies.

### `CorrectionHistory::corrected`

`raw` plus [`correction`](#correctionhistorycorrection), clamped into
`(VALUE_MATED_IN_MAX_PLY, VALUE_MATE_IN_MAX_PLY)` so a corrected static eval can never be
mistaken for a mate score by the pruning guards that test `is_mate`.

**Parameters:** `s` (`const CorrSlots&`), `raw` (`Value`, a side-to-move-relative static
eval).
**Returns:** `Value` — the eval the search should reason about.

### `CorrectionHistory::update`

Teach every table one node's residual with the gravity update:

```
weight = min(depth + 1, MAX_WEIGHT)
bonus  = clamp(diff · CORR_GRAIN · weight / BONUS_DIV, ±MAX_BONUS)
entry  = clamp(entry + bonus − entry · |bonus| / CORR_LIMIT, ±CORR_LIMIT)   // every table
```

A deeper node's disagreement is more trustworthy, so it moves the entries further. The
damping term shrinks steps as an entry nears its limit, so a saturated entry still yields
to residuals of the other sign. The continuation entry is skipped when `s.contPiece` is
`-1`. Tables with weight `0` are updated too (they are just never read).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `s` | `const CorrSlots&` | The node's buckets, as hashed for its static eval. |
| `depth` | `int` | Remaining depth of the node that produced `diff`; weights the update. |
| `diff` | `Value` | Residual `searchScore − correctedEval` in cp, against the **corrected** eval the node actually used — so the tables learn what is still missing and their sum converges on the true residual. |

**Side Effects:** writes five or six `int16_t` buckets.

**Warnings:** the caller
([`Searcher::search`](search.hpp.md#searchersearch)) decides *when* a residual is
trustworthy, and those guards are part of the contract: not in check, a real **quiet**
best move (a capture's score reflects material, not the position's character), no mate
scores, not inside a singular-verification search, and the bound must not contradict the
residual's sign (a `BOUND_LOWER` node may only teach that the eval was too **low**, a
`BOUND_UPPER` node only that it was too **high**).
