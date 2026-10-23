# Module: `viriformat`

The binary encoder for **viriformat**, the self-play game format that the `bullet` NNUE
trainer's `ViriBinpackLoader` reads ([PLAN.md](../PLAN.md) §20). It turns a
`chess::Board` start position plus the `(move, score)` sequence of a game into exactly the
bytes the Rust [`viriformat`](https://crates.io/crates/viriformat) crate (v2.0.1)
deserialises. Used only by [datagen](datagen.hpp.md); it is not on any search path and
allocates freely (one buffer per worker, reused across games).

Viriformat stores **whole games** rather than individual positions: a 32-byte
*marlinformat* packed start position followed by one 4-byte entry per ply. That makes it
roughly 8× smaller than per-position formats, and because every position is kept, the
position filters (minimum ply, in-check, tactical moves, eval cap) are applied at
**train time** by bullet, so changing them never requires regenerating data.

## Source Files

- **Header (interface):** [viriformat.hpp](viriformat.hpp)
- **Source (implementation):** [viriformat.cpp](viriformat.cpp)

## Related Test Documentation

- [test_viriformat.cpp.md](../tests/test_viriformat.cpp.md)

## Namespace

- Public API in namespace `engine::viriformat`.
- The byte offsets (`OCCUPANCY_OFFSET` … `WDL_OFFSET`), the code constants
  (`UNMOVED_ROOK`, `COLOR_SHIFT`, `STM_SHIFT`, `NO_EP_SQUARE`, `MAX_HALFMOVE`), the move
  bit layout (`TO_SHIFT`, `PROMO_SHIFT`, `FLAG_EP`, `FLAG_CASTLE`, `FLAG_PROMO`),
  `TERMINATOR_BYTES`, `SCORE_MAX`, and the helpers `putU16`, `putU64`, `writeU16`,
  `isCastlingRook`, and `pieceCode` live in an anonymous namespace in `viriformat.cpp` —
  internal linkage, not part of the interface.

## Format reference

Every multi-byte field is **little-endian**, written byte by byte so the output does not
depend on the host's endianness.

**Game framing** — games are simply concatenated, so per-thread files can be joined with
`cat`:

| Bytes | Content |
|-------|---------|
| 32 | Packed start position (below), with the game's outcome in its WDL byte. |
| 4 × *n* | One entry per ply played: `u16` move (below), then `i16` score. |
| 4 | Terminator: an all-zero entry (a zero move is impossible, since `A1→A1` is never a move). |

The **score** is the search score of the position the move was played from, in
centipawns, **from White's point of view** (the crate's eval-vs-result filter reads it
that way), clamped to `±32767`. Mate scores (`±(32000 − ply)`, see
[types.hpp.md](types.hpp.md)) fit as-is; bullet's default filter drops `|eval| ≥ 31339`.

**Packed board (marlinformat, 32 bytes):**

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | Occupancy bitboard (`A1` = bit 0). |
| 8 | 16 | 32 4-bit piece codes, one per occupied square in ascending square order; code *i* is the low nibble of byte `8 + i/2` when *i* is even, the high nibble when odd. Code = piece type (`P=0 N=1 B=2 R=3 Q=4 K=5`, or `6` for an *unmoved rook*) `\|` colour `<< 3` (White 0, Black 1). |
| 24 | 1 | Side to move `<< 7` `\|` en-passant square (`64` when there is none). |
| 25 | 1 | Fifty-move (halfmove) clock, saturated at 255. |
| 26 | 2 | Fullmove number (`u16`). |
| 28 | 2 | Eval (`i16`) — always `0` here; the per-ply scores carry the labels. |
| 30 | 1 | WDL, from White's view: [`Outcome`](#enum-class-outcome). |
| 31 | 1 | Extra — always `0`. |

Marlinformat has **no castling field**: a rook that still carries a castling right is
stored with piece code `6` (*unmoved rook*). Which right it carries is decided by the side
of its own king it stands on — exactly the rule the crate's `pack`/`unpack` use, so
Chess960-style rook files round-trip too.

**Move (`u16`):** `from | to << 6 | promo << 12 | flag << 14`, with `flag` = `0b00`
normal, `0b01` en passant, `0b10` castling, `0b11` promotion, and `promo` counted from
the knight (`N=0 B=1 R=2 Q=3`). Castling is encoded as **king takes own rook**
(`e1h1` for `O-O`) — the same convention the Disservin library uses internally, so a
castling move only needs its flag translated.

## Objects / Interfaces

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `PACKED_BOARD_BYTES` | `32` | Size of a marlinformat packed board. |

### `using PackedBoard = std::array<uint8_t, PACKED_BOARD_BYTES>`

One marlinformat packed position, laid out as in the [format reference](#format-reference).

### `enum class Outcome`

A game result from **White's** point of view, valued as marlinformat's WDL byte.

| Enumerator | Value | Meaning |
|------------|-------|---------|
| `BlackWin` | `0` | Black won (mate, or a winning adjudication for Black). |
| `Draw` | `1` | Draw by rule (stalemate, repetition, fifty moves, insufficient material) or a draw adjudication. |
| `WhiteWin` | `2` | White won. |

**Used by:** [`packBoard`](#packboard), [`Game::write`](#gamewrite), [`Adjudicator::update`](datagen.hpp.md#adjudicatorupdate)

### `class Game`

One self-play game being recorded: the packed start position and a `(move, score)` entry
per ply. A [datagen](datagen.hpp.md) worker holds a single `Game` for its whole run and
calls [`reset`](#gamereset) at each new game, so the entry vector's capacity is reused.

| Field | Type | Description |
|-------|------|-------------|
| `start_` | [`PackedBoard`](#using-packedboard--stdarrayuint8_t-packed_board_bytes) | The start position, packed once at `reset` with a placeholder `Draw` outcome; [`write`](#gamewrite) stamps the real one into a copy. |
| `entries_` | `std::vector<Entry>` | One `Entry { uint16_t move; int16_t score; }` per ply: the encoded move and the clamped white-relative score. |

**Methods:** [`Game::reset`](#gamereset), [`Game::addMove`](#gameaddmove),
[`Game::positions`](#gamepositions), [`Game::write`](#gamewrite).

**Used by:** [datagen](datagen.hpp.md)

## Functions

### `packBoard`

Pack `board` into marlinformat.

**When to use:** to write a game header; [`Game::reset`](#gamereset) calls it.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `board` | `const Board&` | Position to pack. Not modified. |
| `outcome` | [`Outcome`](#enum-class-outcome) | Written to the WDL byte (offset 30). |

**Returns:** [`PackedBoard`](#using-packedboard--stdarrayuint8_t-packed_board_bytes) — the
32 bytes of the [format reference](#format-reference), eval and extra zero.

**Side Effects:** none (pure).

**Warnings:** the en-passant byte is whatever `board.enpassantSq()` reports; the
Disservin library only sets it when an en-passant capture is actually available, which
the crate accepts.

### `encodeMove`

Return the 16-bit viriformat encoding of a `chess::Move` (see the
[move layout](#format-reference)). Normal, en-passant, castling, and promotion moves map
field by field; promotions renumber the piece from the knight. Pure.

### `Game::reset`

Start recording a new game from `start` (`const Board&`, not modified): packs it into
`start_` and clears `entries_` (keeping their capacity).

### `Game::addMove`

Append one ply: `move` (`Move`, encoded with [`encodeMove`](#encodemove)) and `whiteScore`
(`Value`, centipawns from White's view, clamped to `±32767`). Record the move together
with the score of the position it was played **from**, then make it — bullet replays the
game in the same order.

### `Game::positions`

Return `std::size_t` — the number of entries (= positions, one per ply) recorded since
the last [`reset`](#gamereset). Trivial getter.

### `Game::write`

Serialise the game to `out`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `out` | `std::ostream&` | Destination (in/out); open it in **binary** mode. Bytes are appended at the current position. |
| `outcome` | [`Outcome`](#enum-class-outcome) | The final result, stamped into the header's WDL byte. |

**Returns:** `void`.

**Side Effects:** writes `32 + 4·positions() + 4` bytes to `out`. Does not flush.

**Warnings:** a game is written in several `write` calls, so a process killed mid-call
can leave a truncated final game; the crate then reports an unexpected EOF for that file's
tail. [datagen](datagen.hpp.md) flushes after every game to keep that window small.
