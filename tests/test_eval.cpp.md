# Test: `test_eval.cpp`

Unit tests for the hand-crafted [evaluation](../src/eval.hpp.md) — the tapered PSQT
eval. They pin the invariants a PSQT + tapered eval must hold **regardless of the
exact (tunable) weights**, so they stay valid after Texel tuning refits the tables:
colour symmetry, a level symmetric position, material dominance, and a PSQT
preference for a centralised piece over a cornered one. Deterministic; `tempo = 0`
throughout so the material/positional signal is read cleanly.

## Test Code

- **Test Code:** [test_eval.cpp](test_eval.cpp)
- **Module under test:** [eval.hpp.md](../src/eval.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Helpers

- `mirrorFen(fen)` returns the vertical-mirror / colour-swap of a position with the
  side to move flipped — the "same" position with the colours exchanged. Castling and
  en passant are dropped (the eval ignores them), keeping the helper trivially
  correct.
- `eval(fen)` is `evaluate(Board(fen), 0)` — the side-to-move-relative eval with no
  tempo.

## Tests

### `evaluation is colour-symmetric`

For several positions (an open game, lopsided minors, a pawns-only ending),
`eval(pos) == eval(mirrorFen(pos))`. Because the eval is side-to-move-relative, a
position and its colour-swapped mirror score **equal** from each mover's own
perspective. A failure is an orientation bug in the `^ 56` table mapping.

### `a symmetric position is level`

The start position is its own mirror, so the side-relative eval is exactly the tempo
— `0` here.

### `a queen up is a large advantage`

With an extra queen for the mover, the eval exceeds `+700` cp — material dominates
the score.

### `PSQT prefers a centralised knight to a cornered one`

Same material (White up a knight, symmetric kings), knight on `d4` vs `a1`: the PSQT
must score the centralised knight strictly higher.

### `pawn structure penalizes doubled and isolated pawns`

Equal material (two White pawns): a healthy connected shape (`c2`/`d2`) versus a
damaged one (`d2`/`d3`, doubled on the d-file and both isolated). The structure
penalties must make the damaged shape score lower despite its slightly more advanced
pawn.

### `pawn structure rewards an advanced passed pawn`

With no enemy pawns both White pawns are passed; the far-advanced `d6` (near
promotion) must score well above the home `d2` via the rank-scaled passed bonus.

### `piece terms reward a rook on an open file`

Equal material (rook + one pawn). With the pawn off the rook's file the a-file is open
(open-file bonus); with the pawn on `a2` it is blocked. The open-file rook must score
higher — isolates the rook open/semi-open term.

### `piece terms reward the bishop pair`

Two bishops vs bishop + knight (near-equal material): the bishop-pair bonus plus the
bishop's edge over the knight must favor the pair.

**On mobility:** the mobility term has no dedicated case — it cannot be isolated
through the full eval (any blocker is itself a piece with its own PSQT and pawn
structure). Its sign/orientation is guarded by the colour-symmetry case, and its net
value by the A/B SPRT.
