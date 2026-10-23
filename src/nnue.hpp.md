# Module: `nnue`

NNUE (Efficiently Updatable Neural Network) evaluation, Phase 2 of the roadmap
([PLAN.md](../PLAN.md) §19–20). It runs a `(768 → 512)×2 SCReLU → 1` network trained by
`bullet` on the engine's own self-play data ([datagen](datagen.hpp.md)). It loads the
trainer's quantised network file, builds and incrementally updates the two hidden-layer
**accumulators** (one per perspective) as the [search](search.hpp.md) makes moves, and
turns them into a centipawn score. It replaces the hand-crafted [`evaluate`](eval.hpp.md#evaluate)
whenever a network is loaded (UCI `EvalFile`, see [uci](uci.hpp.md)); the HCE stays as the
fallback when none is.

`update` and `evaluate` are on the **per-node hot path**. They are plain fixed-width
loops over 512 `int16` lanes that the compiler auto-vectorises (`-O3 -march=native`). They
don't allocate and do no I/O. Measured on the 5900X, an NNUE search runs at about
**3.05M nps against 3.24M nps** for the HCE (startpos, depth 14), so hand-written AVX2
kernels are deferred until a bigger net makes inference the bottleneck.

## Source Files

- **Header (interface):** [nnue.hpp](nnue.hpp)
- **Source (implementation):** [nnue.cpp](nnue.cpp)

## Related Test Documentation

- [test_nnue.cpp.md](../tests/test_nnue.cpp.md)

## Namespace

- Public API in namespace `engine::nnue`; the header also brings `chess::Color` into
  `engine` (`using chess::Color;`, as [history](history.hpp.md) does).
- Internal linkage (anonymous namespace in `nnue.cpp`): `NUM_SQUARES`, `THEIR_OFFSET`,
  `FLIP_RANKS`, `NETWORK_BYTES`, the layout `static_assert`s, the `Feature` / `Delta` structs,
  and the helpers `featureIndex`, `moveDelta`, `applyDelta`, `screlu`.

## The network

**Input features (bullet's `Chess768`).** Each perspective encodes every piece as one of
768 inputs: `side · 384 + pieceType · 64 + square`, where `side` is 0 for that perspective's
own pieces and 1 for the opponent's, `pieceType` is `P N B R Q K = 0…5`, and the square is
mirrored vertically (`sq ^ 56`) for Black's view. So the White and Black accumulators of a
position and of its colour-flipped twin are identical. The trainer works from the side to
move's point of view, so `evaluate` reads the mover's accumulator first.

**Forward pass.** `acc[p] = featureBias + Σ featureWeights[f]` over the active features of
perspective `p`. Then:

```
out = ( Σ SCReLU(acc[stm][i]) · w[i]  +  Σ SCReLU(acc[!stm][i]) · w[512 + i] ) / QA
      + outputBias
eval = out · SCALE / (QA · QB)          SCReLU(x) = clamp(x, 0, QA)²
```

**File layout** (a bullet `quantised.bin`, little-endian `int16`, read straight into
[`Network`](#struct-network)):

| Offset (bytes) | Block | Shape | Quantisation |
|---|---|---|---|
| 0 | `featureWeights` | 768 columns × 512 | `QA` (255) |
| 786,432 | `featureBias` | 512 | `QA` |
| 787,456 | `outputWeights` | 2 × 512 (own, then theirs) | `QB` (64) |
| 789,504 | `outputBias` | 1 | `QA · QB` |
| 789,506 | padding | to a multiple of 64 (file is 789,568 bytes) | — |

The engine's integer eval tracks the trainer's float eval to within quantisation error. For
net1 (startpos, either queen missing, an Italian opening) the engine gives
`33 / 1378 / −1311 / 43` cp against the trainer's `31.0 / 1380.2 / −1316.1 / 33.6`.

## Objects / Interfaces

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `INPUTS` | `768` | Input features per perspective (2 sides × 6 piece types × 64 squares). |
| `HIDDEN` | `512` | Accumulator width per perspective. |
| `QA` | `255` | Feature-layer quantisation; also the SCReLU clamp. |
| `QB` | `64` | Output-layer quantisation. |
| `SCALE` | `400` | Network output → centipawns (the trainer's `eval_scale`). |

These must match the trainer the network came from (`C:\Users\sivuc\nnue-data\trainer`,
see [PLAN.md](../PLAN.md) §20). A different net shape needs these and the file size to change.

### `struct Network`

The quantised weights, laid out exactly like the file. Every block is a multiple of 64 bytes,
so `alignas(64)` adds no interior padding and the `static_assert`s in `nnue.cpp` pin each
offset. About 790 KB; always heap-allocated (by [`loadNetwork`](#loadnetwork)).

| Field | Type | Description |
|-------|------|-------------|
| `featureWeights` | `std::array<std::array<int16_t, HIDDEN>, INPUTS>` | One 512-wide column per input feature. |
| `featureBias` | `std::array<int16_t, HIDDEN>` | Hidden-layer bias; every accumulator starts from it. |
| `outputWeights` | `std::array<int16_t, 2 * HIDDEN>` | First half weights the side to move's activations, second half the opponent's. |
| `outputBias` | `int16_t` | Output bias, in `QA · QB` units. |

**Used by:** [`Searcher`](search.hpp.md#class-searcher) (as `const Network*`, not owned),
the [uci](uci.hpp.md#class-engine-internal) `Engine` (owns it).

### `struct Accumulator`

The hidden layer of one position from both perspectives: `values[c]` (`std::array<int16_t,
HIDDEN>`, indexed by `chess::Color`) is perspective `c`'s 512 pre-activation sums. 2 KB.
The [`Searcher`](search.hpp.md#class-searcher) keeps one per ply.

## Functions

### `loadNetwork`

Read a bullet `quantised.bin`.

**Parameters:** `path` (`const std::string&`) — the file.

**Returns:** `std::unique_ptr<Network>` — the network, or `nullptr` when the file can't be
opened, is shorter than the 789,506 data bytes, or the read fails. Trailing bytes (the
trainer's padding) are ignored.

**Side Effects:** file I/O. Cold path: called once per `EvalFile` change.

**Warnings:** there is no header or checksum, so any file of the right size loads. The
format assumes a little-endian host (x86).

### `refresh`

Build `out` (`Accumulator&`, out) for `board` (`const Board&`) from scratch: start each
perspective from `featureBias` and add the column of every piece. Used once per search at
the root and by the tests as the reference for [`update`](#update).

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
over the 512 lanes from `parent` into `child`.

**Performance:** per make in the search; branch-light, no allocation.

### `evaluate`

Return [`Value`](types.hpp.md#using-value--int) — the network's score of the position `acc`
describes, in centipawns from `stm`'s point of view (`Color`), clamped clear of the mate
range. Applies SCReLU to both perspectives, takes the dot product with `outputWeights`, adds
the bias, and rescales (see [The network](#the-network)).

**Performance:** per node; 1024 multiply-adds into an `int32` sum.

**Warnings:** the sum is `int32`, like bullet's reference inference. Its theoretical worst
case (every lane saturated against maximal weights) exceeds the range, but trained networks
stay far below it.
