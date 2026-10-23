# Test: `test_viriformat.cpp`

Unit tests for the [viriformat encoder](../src/viriformat.hpp.md) (`src/viriformat.*`).
They are **known-answer byte checks** against the marlinformat / viriformat layout that the
Rust `viriformat` crate (and so `bullet`'s `ViriBinpackLoader`) decodes: the packed start
position, the 16-bit move encoding of every move type, and the framing of a serialised
game. Expected bytes are derived by hand from the
[format reference](../src/viriformat.hpp.md#format-reference); a local `nibble` helper
reads piece code *i* back out of a packed board, `occupancy` reassembles the little-endian
bitboard, and `moveBits` encodes a UCI move parsed on a given FEN.

These pin the encoding against regressions. The end-to-end proof that the bytes really
decode was a separate run: 203,590 positions from a `chessai datagen` run, replayed with
the crate itself (every move legal, outcomes consistent) and trained on through bullet
(see [datagen.hpp.md](../src/datagen.hpp.md)).

## Test Code

- **Test Code:** [test_viriformat.cpp](test_viriformat.cpp)
- **Module under test:** [viriformat.hpp.md](../src/viriformat.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `startpos packs to the reference marlinformat bytes`

The standard start position packs to occupancy `0xFFFF00000000FFFF`. Its 32 piece nibbles
are White's back rank as `6 1 2 4 5 2 1 6` (both rooks carry castling rights, so they get
the unmoved-rook code 6), eight white pawns (`0`), eight black pawns (`8`, colour bit
set), and Black's back rank as `14 9 10 12 13 10 9 14`. Then: byte 24 = `64` (White to
move, no en passant), halfmove `0`, fullmove `1` (LE), eval `0`, WDL `2` for `WhiteWin`,
extra `0`.

### `only rooks that still carry a castling right get the unmoved-rook code`

In `r3k2r/8/8/8/8/8/8/R3K2R w K - 0 1` only White's kingside right remains: `h1` packs as
6, `a1` as a plain rook (3), and both black rooks as plain black rooks (11). WDL byte `1`
for `Draw`.

### `side to move, en passant, and clocks land in their bytes`

A position with a live en-passant capture on `f6` packs byte 24 as `45` (the square, White
to move). A black-to-move king ending with clocks `37 60` packs byte 24 as `1 << 7 | 64`,
halfmove `37`, fullmove `60`, and WDL `0` for `BlackWin`.

### `moves encode as from | to << 6 | promo << 12 | flag << 14`

`e2e4` → `12 | 28 << 6`. White `O-O` (`e1g1`) → `4 | 7 << 6` with the castle flag, i.e.
**king takes own rook**; Black `O-O-O` (`e8c8`) → `60 | 56 << 6` with the castle flag.
`e5f6` en passant → `36 | 45 << 6` with the EP flag. `a7a8q` → promo `3` and the promotion
flag; `a7a8n` → promo `0`.

### `a game serialises as header, LE entries, and a zero terminator`

A two-ply game (`e2e4` scored −150, then a move scored 40000) written as `BlackWin` is
exactly `32 + 2·4 + 4` bytes: the outcome is stamped into header byte 30, the first entry
reads back as the encoded `e2e4` and `−150` (signed, little-endian), the 40000 score is
clamped to `32767`, and the final four bytes are zero. A following `reset` empties the
entry list.
