# Module: `nnue`

NNUE (Efficiently Updatable Neural Network) evaluation, Phase 2 of the roadmap
([PLAN.md](../PLAN.md) §19–20). It runs a `(768hm → 1024)×2 SCReLU → 8` network trained
by `bullet` on self-play positions ([datagen](datagen.hpp.md)): **horizontally mirrored
inputs** (each perspective mirrors the board a↔h when its own king is on files e–h), one
1024-wide hidden layer, and eight **output buckets** (separate output layers) picked by
how many pieces are on the board. The code also supports **king input buckets** (the
perspective's 768 inputs chosen from several sets by its king square); the current net
uses a single bucket, because 10 king buckets lost Elo on our ~400M training positions. It loads the trainer's quantised network file, builds and
incrementally updates the two hidden-layer **accumulators** (one per perspective) as the
[search](search.hpp.md) makes moves, and turns them into a centipawn score. **It is the default eval:** the build links a network
into the binary ([`embeddedNetwork`](#embeddednetwork), see
[The embedded network](#the-embedded-network)), which [uci](uci.hpp.md),
[bench](bench.hpp.md) and [datagen](datagen.hpp.md) all use; UCI `EvalFile` loads another
one in its place. The hand-crafted [`evaluate`](eval.hpp.md#evaluate) remains the fallback
(UCI `UseNNUE false`, or a build that embeds no network).

`update` and `evaluate` are on the **per-node hot path**. They don't allocate and do no
I/O. `update` is a plain fixed-width loop over 1024 `int16` lanes that the compiler
auto-vectorises (`-O3 -march=native`). `evaluate`'s output layer is hand-written AVX2 in
an AVX2 build (the "lizard" SCReLU, see [`evaluate`](#evaluate)) and a portable loop
otherwise. With the first network (`(768 → 512)×2 → 1`, net1) an NNUE search ran at about
**3.05M nps against 3.24M nps** for the HCE on the 5900X (startpos, depth 14). Doubling the
width to 1024 for net2 doubles the per-node accumulator and output work: `bench` dropped
from about 2.7M to 2.2M nps on an otherwise idle machine. The AVX2 output layer then won
back **+20% nps**.

## Source Files

- **Header (interface):** [nnue.hpp](nnue.hpp)
- **Source (implementation):** [nnue.cpp](nnue.cpp)

## Related Test Documentation

- [test_nnue.cpp.md](../tests/test_nnue.cpp.md)

## Namespace

- Public API in namespace `engine::nnue`; the header also brings `chess::Color` into
  `engine` (`using chess::Color;`, as [history](history.hpp.md) does).
- Internal linkage (anonymous namespace in `nnue.cpp`): `NUM_SQUARES`, `THEIR_OFFSET`,
  `NUM_FILES`, `HALF_FILES`, `FLIP_RANKS`, `FLIP_FILES`, `MAX_PIECES`, `MIN_PIECES`,
  `PIECES_PER_BUCKET`, `MAX_OUTPUT_WEIGHT`, `FEATURE_COLUMNS`, `NETWORK_BYTES`, the layout
  `static_assert`s, the `Feature` / `Delta` / `View` structs, and the helpers `viewOf`,
  `featureIndex`, `moveDelta`, `kingAfter`, `removedBy`, `addColumn`, `rebuild`,
  `applyDelta`, `outputBucket`, `activatedDot`, `outputWeightsInRange`, `checked`. With AVX2 (`__AVX2__`) there is also `horizontalSum`;
  without it there is `screlu`.
- At global scope in `nnue.cpp`, only when the network is embedded: the top-level `asm`
  block and the `extern "C"` symbols `chessai_net_begin` / `chessai_net_end` it defines,
  plus the `CHESSAI_HAS_EMBEDDED_NET` build flag. They are not declared in the header;
  [`embeddedNetwork`](#embeddednetwork) is the only way in.

## The embedded network

The default network is linked into the executable so the engine is one self-contained
file ([PLAN.md](../PLAN.md) §0.4). The build passes its absolute path as the `EVALFILE`
string macro: the [Makefile](../build.md#nnue-network) sets `EVALFILE ?= nets/net3.bin`
(OpenBench overrides it with `make EVALFILE=<net>`), and CMake has an `EVALFILE` cache path.
A top-level `asm` block then `.incbin`s the file's bytes into read-only data
(`.rodata` on ELF, `.rdata` on Windows/COFF), 64-byte aligned, between two global labels.

`.incbin` is used instead of `#embed` because the MinGW GCC 13 that builds the Windows
releases has no `#embed` (and Clang only offers it as an extension). MSVC has no GNU-style
top-level `asm`, so an MSVC build, or any build without `EVALFILE`, embeds nothing:
`embeddedNetwork()` returns `nullptr` and the engine uses the HCE unless `EvalFile` loads a
file. The default network is `nets/net3.bin`, committed to the repository
(1,607,744 bytes, SHA-256 `6a03098c…3d16311e`); the **bench signature with it is 46936
nodes** (42410 with net2). net3 keeps net2's shape and changes the training data: its
targets are **Stockfish scores**. The generation-3 self-play positions (run3: 114M
positions of net2 self-play) were rescored by Stockfish 19 at 5,000 nodes each, scaled
by 1.25 onto the engine's centipawn scale, and counted twice; run2 (300M, net1 self-play)
was added with its own search scores. 60 superbatches took 1 h 11 min on the RX 6800 XT
(final loss 0.0117). A 1536-wide net with 10 king buckets, trained on net2's own data, lost
Elo and was not shipped.

## The network

**Input features (bullet's `ChessBucketsMirrored`).** Each perspective first fixes its
**view** from its own king square `k`, seen from its side (`k ^ 56` for Black):

- **Mirror:** when `k` is on files e–h, every square is mirrored a↔h (`sq ^ 7`), so the
  king always sits on files a–d.
- **Bucket:** the (mirrored) king square picks an input bucket from `KING_BUCKET_LAYOUT`
  (4 files × 8 ranks). With the current single-bucket layout (all zeros) every king square
  maps to bucket 0, so only the mirroring changes the view.

Each piece is then one of `INPUT_BUCKETS × 768` inputs (768 for the current net):
`bucket · 768 + side · 384 + pieceType · 64 + (sq ^ rankFlip ^ fileFlip)`, where `side` is
0 for the perspective's own pieces and 1 for the opponent's and `pieceType` is
`P N B R Q K = 0…5`. So a position's colour-flipped twin and its left-right mirror (without
castling rights) give identical accumulators. The trainer works from the side to move's
point of view, so `evaluate` reads the mover's accumulator first.

**Output bucket.** The position's piece count `n` (both kings included, 2…32) picks one of
the eight output layers: `bucket = (n − 2) / 4`, so 2–5 pieces use bucket 0, 6–9 bucket 1,
and so on up to 30–32 in bucket 7. This is bullet's `MaterialCount<8>` (it divides by
`ceil(32 / 8) = 4`); the engine must use the same rule the trainer did.

**Forward pass.** `acc[p] = featureBias + Σ featureWeights[f]` over the active features of
perspective `p`. Then, with `w = outputWeights[bucket]`:

```
out = ( Σ SCReLU(acc[stm][i]) · w[i]  +  Σ SCReLU(acc[!stm][i]) · w[1024 + i] ) / QA
      + outputBias[bucket]
eval = out · SCALE / (QA · QB)          SCReLU(x) = clamp(x, 0, QA)²
```

**File layout** (a bullet `quantised.bin`, little-endian `int16`, read straight into
[`Network`](#struct-network)):

| Offset (bytes) | Block | Shape | Quantisation |
|---|---|---|---|
| 0 | `featureWeights` | 768 columns (1 bucket × 768) × 1024 | `QA` (255) |
| 1,572,864 | `featureBias` | 1024 | `QA` |
| 1,574,912 | `outputWeights` | 8 rows × 2048 (each row: own 1024, then theirs 1024) | `QB` (64) |
| 1,607,680 | `outputBias` | 8 | `QA · QB` |
| 1,607,696 | padding | to a multiple of 64 (file is 1,607,744 bytes, the same size as net2's) | — |

The output weights must be **bucket-major** (one contiguous row per bucket). bullet stores
an affine layer's weights the other way round, so the trainer saves them with
`SavedFormat::id("l1w")…transpose()`; without it the engine would read scrambled rows.

The engine's integer eval tracks the trainer's float eval to within quantisation error.
For net3 on startpos, either queen missing, an Italian opening, KQ vs K (bucket 0), both
sides castled short, opposite castling (Black to move) and a king-and-pawn ending with the
kings on g3 / b6 (the last three exercise the a↔h mirroring) the engine gives
`44 / 1646 / −1562 / 63 / 461 / 35 / 173 / −14` cp against the trainer's
`36.5 / 1636.9 / −1573.3 / 58.4 / 459.9 / 38.5 / 171.6 / −14.8`.

## Objects / Interfaces

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `INPUTS` | `768` | Input features per king bucket (2 sides × 6 piece types × 64 squares). |
| `HIDDEN` | `1024` | Accumulator width per perspective. |
| `KING_BUCKET_LAYOUT` | `std::array<int, 32>` | King square (own side's view, mirrored to files a–d; a1–d1 first, rank by rank) → input bucket. All zeros (one bucket) for the current net. Must match the trainer's `BUCKET_LAYOUT`. |
| `INPUT_BUCKETS` | `1` | Number of king input buckets (`static_assert`ed against the layout). |
| `OUTPUT_BUCKETS` | `8` | Output layers; a position's piece count picks one (see [The network](#the-network)). |
| `QA` | `255` | Feature-layer quantisation; also the SCReLU clamp. |
| `QB` | `64` | Output-layer quantisation. |
| `SCALE` | `400` | Network output → centipawns (the trainer's `eval_scale`). |

These must match the trainer the network came from (`C:\Users\sivuc\nnue-data\trainer`,
binary `net3m`, see [PLAN.md](../PLAN.md) §20). A different net shape needs these and the
file size to change; a net of another shape (such as net1) fails to load.

### `struct Network`

The quantised weights, laid out exactly like the file. Every block but the trailing biases
is a multiple of 64 bytes, so `alignas(64)` adds no interior padding and the
`static_assert`s in `nnue.cpp` pin each offset. About 1.6 MB; always heap-allocated (by
[`loadNetwork`](#loadnetwork) or [`networkFromBytes`](#networkfrombytes)).

| Field | Type | Description |
|-------|------|-------------|
| `featureWeights` | `std::array<std::array<int16_t, HIDDEN>, INPUT_BUCKETS * INPUTS>` | One 1024-wide column per input feature, bucket-major (bucket `b`'s 768 columns start at `b · 768`). |
| `featureBias` | `std::array<int16_t, HIDDEN>` | Hidden-layer bias; every accumulator starts from it. |
| `outputWeights` | `std::array<std::array<int16_t, 2 * HIDDEN>, OUTPUT_BUCKETS>` | One row per output bucket. In each row the first half weights the side to move's activations, the second half the opponent's. |
| `outputBias` | `std::array<int16_t, OUTPUT_BUCKETS>` | One output bias per bucket, in `QA · QB` units. |

**Used by:** [`Searcher`](search.hpp.md#class-searcher) (as `const Network*`, not owned).
Owned either by [`embeddedNetwork`](#embeddednetwork)'s static (the default) or by the
[uci](uci.hpp.md#class-engine-internal) `Engine` (one loaded through `EvalFile`).

### `struct Accumulator`

The hidden layer of one position from both perspectives: `values[c]` (`std::array<int16_t,
HIDDEN>`, indexed by `chess::Color`) is perspective `c`'s 1024 pre-activation sums. 4 KB.
The [`Searcher`](search.hpp.md#class-searcher) keeps one per ply.

## Functions

### `loadNetwork`

Read a bullet `quantised.bin`.

**Parameters:** `path` (`const std::string&`) — the file.

**Returns:** `std::unique_ptr<Network>` — the network, or `nullptr` when the file can't be
opened, is shorter than the 1,607,696 data bytes, the read fails, or an output weight is
outside `±MAX_OUTPUT_WEIGHT` (see [`evaluate`](#evaluate)). Trailing bytes (the trainer's
padding) are ignored.

**Side Effects:** file I/O. Cold path: called once per `EvalFile` change.

**Warnings:** there is no header or checksum, so any file of the right size loads. The
format assumes a little-endian host (x86).

### `networkFromBytes`

Copy a network out of a buffer holding a `quantised.bin`'s contents.

**Parameters:** `data` (`const unsigned char*`) — the bytes, not retained; `size`
(`std::size_t`) — their count.

**Returns:** `std::unique_ptr<Network>` — a heap copy (so it is correctly aligned whatever
`data`'s alignment), or `nullptr` when `data` is null, holds fewer than the 1,607,696 data
bytes, or has an output weight outside `±MAX_OUTPUT_WEIGHT` (see [`evaluate`](#evaluate)).
Trailing bytes are ignored. **Side Effects:** none beyond the allocation.

### `embeddedNetwork`

Return `const Network*` — the network linked into the binary
([The embedded network](#the-embedded-network)), or `nullptr` when the build embedded none
(or the embedded file is too short). The first call copies the embedded bytes into a
`Network` with [`networkFromBytes`](#networkfrombytes) inside a function-local static, so
initialisation is thread-safe and happens once. Every later call returns the same pointer,
valid for the life of the program and only ever read.

**Used by:** [uci](uci.hpp.md#engineactivenetwork-internal), [bench](bench.hpp.md#run),
[datagen](datagen.hpp.md#class-worker-internal).

### `refresh`

Build `out` (`Accumulator&`, out) for `board` (`const Board&`) from scratch: start each
perspective from `featureBias` and add the column of every piece, indexed through that
perspective's view (its king's bucket and mirroring). Used once per search at the root
and by the tests as the reference for [`update`](#update).

### `update`

Derive a child accumulator incrementally.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `net` | `const Network&` | The network. |
| `board` | `const Board&` | The position **before** `move` (the moved and captured pieces are read from it). |
| `move` | `Move` | The move about to be made. |
| `parent` | `const Accumulator&` | `board`'s accumulator. |
| `child` | `Accumulator&` | Out: the accumulator of the position after `move`. |

**Behavior:** a move switches at most two features on and two off per perspective:

- quiet move: −(piece, from), +(piece, to);
- capture: also −(victim, to);
- en passant: also −(their pawn, `to.ep_square()`);
- promotion: +(promoted piece, to) in place of the pawn;
- castling (encoded by the library as king takes own rook): −king, −rook, +king on its
  castled square, +rook on its castled square.

`applyDelta<ADDS, SUBS>` handles the three shapes (1/1, 1/2, 2/2), each as one fused pass
over the 1024 lanes from `parent` into `child`.

**King moves.** Each perspective's [view](#the-network) depends on its own king. When the
mover's king crosses between files d and e (which flips the mirroring) or, with king
buckets, lands in a different input bucket, every one of that perspective's columns changes, so it is rebuilt
from scratch (`rebuild`: bias plus every piece of the position after the move, read from
the pre-move `board` minus the removed features plus the added ones) instead of
updated. Castling usually triggers this. The opponent's perspective, whose king did not
move, always takes the incremental update.

**Performance:** per make in the search; branch-light, no allocation.

### `evaluate`

Score a position from its accumulator.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `net` | `const Network&` | The network. |
| `acc` | `const Accumulator&` | `board`'s accumulator (from [`refresh`](#refresh) / [`update`](#update)). |
| `board` | `const Board&` | The position. Read for the side to move and, through `outputBucket`, its piece count (`board.occ().count()`). Not modified. |

**Returns:** [`Value`](types.hpp.md#using-value--int) — the network's score in centipawns
from the side to move's point of view, clamped clear of the mate range. It picks the output
bucket from the piece count, applies SCReLU to both perspectives, takes the dot product
with that bucket's `outputWeights` row, adds its bias, and rescales (see
[The network](#the-network)).

**Used by:** [`Searcher::staticEval`](search.hpp.md#searcherstaticeval-private).

**Performance:** per node; 2048 multiply-adds into an `int32` sum, done as two dot products
(`activatedDot`, one per perspective).

- **AVX2 build (the "lizard" SCReLU).** With `v = clamp(x, 0, QA)`, the product `v · w`
  fits `int16` because every output weight is within `±MAX_OUTPUT_WEIGHT = 32767 / QA = 128`
  (bullet clips them to about ±127; net2 and net3 both span exactly ±127). So `vpmullw` forms `v · w`
  16 lanes at a time, and `vpmaddwd` multiplies by `v` again and adds lane pairs into
  `int32`. The result is exactly `v² · w`, without the 8-lane `vpmulld` the plain form
  needs. `horizontalSum` folds the eight `int32` lanes at the end. Measured on the 5900X:
  `evaluate` drops from about 130 to 56 ns per call, and 4M-node searches run **+20% nps**
  (median of 15 interleaved pairs over three positions, against v0.3).
- **Portable build.** The plain `Σ screlu(x) · w` loop, auto-vectorised. It is written as a
  single-stream reduction per perspective because MinGW GCC 13 (the Windows release
  compiler) left the two-perspective form scalar (~0.97M against ~3.0M nps).

Both forms give identical scores (integer addition is associative), so the bench signature
is unchanged.

**Warnings:** the sum is `int32`, like bullet's reference inference. Its theoretical worst
case (every lane saturated against maximal weights) exceeds the range, but trained networks
stay far below it. The lizard form is only exact for output weights within
`±MAX_OUTPUT_WEIGHT`. The loaders enforce this, so a network built some other way must
respect it too.
