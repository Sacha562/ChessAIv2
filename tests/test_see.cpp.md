# Test: `test_see.cpp`

Unit tests for [Static Exchange Evaluation](../src/see.hpp.md) (`src/see.*`). Each
case plays out a **hand-verified** capture sequence on a crafted position and pins the
exact SEE value by **bracketing the threshold**: `seeGE(m, v)` is `true` and
`seeGE(m, v + 1)` is `false`, so SEE `== v`. The suite targets the classic SEE bug
surface — x-ray / battery reveals, en passant, promotion, and the king rule. Because
SEE is minimax (a side declines a losing recapture), a value never exceeds the first
captured piece, which the brackets respect.

## Test Code

- **Test Code:** [test_see.cpp](test_see.cpp)
- **Module under test:** [see.hpp.md](../src/see.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `pieceValue maps the material scale`

`pieceValue` returns the expected centipawns per piece type, and `PieceType::NONE`
maps to `0`.

### `free capture of an undefended piece`

Rook takes an undefended pawn: SEE `== +100` (no recapture).

### `equal exchange nets zero`

`Nf3xe5` into a pawn recapture: `320 − 320 == 0`.

### `losing capture: rook takes a pawn-defended pawn`

`Re1xe5` winning a pawn but losing the rook to `...d6xe5`: `100 − 500 == −400`.

### `battery / x-ray: doubled rooks recover the exchange`

`Re2xe5` with a second rook behind it: the x-ray rescan reveals the rear rook, so the
exchange nets `+100` instead of the single-attacker `−400`.

### `en passant capture`

`e5xd6 e.p.` removes the pawn on its real square (`d5`), not `d6`: SEE `== +100`.

### `promotion capture scores the queening bonus`

`b7xa8=Q` takes a knight and promotes: `320 + (900 − 100) == 1120`.

### `king may not recapture into a defended square`

`d4xe5` where the black king "defends" e5 but a white pawn also covers it — the king
cannot legally take back, so SEE `== +100`.

### `king recapture allowed when the square is undefended`

The same position without the covering pawn: `...Kxe5` is legal, so `100 − 100 == 0`.
Together with the previous case this pins the king rule from both sides.

### `quiet non-capture into a defended square is losing`

A rook stepping onto a pawn-guarded square (a quiet move, victim value 0) yields
SEE `< 0` — exercising the non-capture path.
