# Tool: `extract` (PGN → labeled-FEN dataset)

Offline tool that turns self-play PGNs into the labeled-position dataset the
[Texel tuner](tune.cpp.md) consumes. It is a standalone command-line program
(`extract`), **not** part of the engine, and is **dependency-free**: it replays games
with the vendored Disservin library's PGN parser and SAN mover, so no Python /
python-chess is required. Header-only — it links nothing from `src/`.

## Source

- **Source:** [extract.cpp](extract.cpp)
- **Build target:** `extract` (see [build.md](../build.md#tuning-tools)) —
  `make extract` or the CMake `extract` target.
- **Feeds:** [tune.cpp.md](tune.cpp.md) (the dataset format is `FEN result` per
  line).

## What it does

Implements a `chess::pgn::Visitor` that, for each game:

1. Starts from `startpos` (or the `[FEN]` header, for book-opening games).
2. Replays each SAN move via `chess::uci::parseSan`, tracking the board.
3. **Emits the pre-move position** when it is a good training sample — that is:
   - past the opening (ply ≥ `--skip-plies`, default 8),
   - the side to move is **not in check**, and
   - the move played is **neither a capture nor a promotion** (tactical moves mark
     positions where the static eval is unreliable).
4. On game end, writes every buffered position as `FEN result`, where `result` is the
   game outcome from **White's** perspective: `1-0 → 1.0`, `0-1 → 0.0`,
   `1/2-1/2 → 0.5`. Games with an undecided result (`*`) are dropped, as is the
   remainder of any game containing an unparseable move.

The quiet/opening/decorrelation filters keep the dataset close to the positions the
eval is actually read on and reduce the strong autocorrelation between consecutive
plies of one game.

## Usage

```
extract <pgn> [--skip-plies N] > dataset.txt      (N default 8)
```

| Argument | Default | Meaning |
|----------|---------|---------|
| `<pgn>` | — | Input PGN (e.g. a fastchess `-pgnout` file). |
| `--skip-plies N` | 8 | Skip the first `N` plies of each game (opening noise). |

The dataset goes to **stdout**; a one-line summary (`positions from games`) goes to
stderr.

## Notes & limitations

- Pairs with [tune.cpp.md](tune.cpp.md): `extract` builds the dataset, `tuner` fits
  the tables to it. Both are documented as one Phase 1c workflow in
  [build.md](../build.md#tuning-tools).
- The label is the **game** result, not a per-position eval — Texel's method relies on
  aggregating many games, so the dataset should come from **many** self-play games
  (tens of thousands), not a handful.
- SAN replay depends on well-formed PGNs. A malformed/unparseable move ends sampling
  for that game (the already-buffered positions are still emitted with the result).
- Reusable beyond tuning: the same replay-and-sample skeleton is the basis for NNUE
  self-play data generation in Phase 2.
