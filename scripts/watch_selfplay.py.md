# Tooling: `watch_selfplay.py`

The **watchable** counterpart to [match.py](match.py). It makes one UCI engine play
itself and writes a **single self-contained `.html` file** — an animated board you
open in a browser and step through. Where [match.py](match.py) runs fastchess for
headless SPRT/regression, this produces one game you can actually watch.

Dependency-free: it talks UCI to the engine and reconstructs every position from
the engine's own `d` dump, so no chess library is needed. The output HTML embeds
the game plus the viewer (no server, no CDN, works offline by double-click).

This script is developer tooling and never links into the engine binary; it lives
under `scripts/` (excluded from the C++ rules) but, being first-party code,
carries this companion per
[documentation-style-guide.md §2.7](../documentation-style-guide.md#27-tooling-scripts).
Python 3.8+, standard library only.

## Source File

- **Code:** [watch_selfplay.py](watch_selfplay.py)

## How to Build & Run

No build. Run from the repo root **inside WSL2** (where the engine binary lives):

```bash
# 20k nodes/move (deterministic) -> ./selfplay.html
python3 scripts/watch_selfplay.py ./chessai

# stronger search, custom output path
python3 scripts/watch_selfplay.py ./chessai --nodes 50000 -o /mnt/c/Users/me/game.html

# fixed time per move instead of nodes
python3 scripts/watch_selfplay.py ./chessai --movetime 500
```

Then open the output file in any browser. Write it under `/mnt/c/...` to
double-click it from Windows. Each run records a fresh game.

The HTML is written **only when the game finishes**, so a live `playing... N plies`
counter is printed to stderr while it runs. `--movetime 2000` × up to `--maxplies`
moves can take minutes; `--nodes` (the default) plays a whole game in ~1–2 s and is
the right choice unless you specifically want long thinking time.

## Options

| Arg | Default | Purpose |
|-----|---------|---------|
| `engine` (positional) | — | Path to the UCI binary (e.g. `./chessai`). |
| `--nodes N` | `20000` | Fixed nodes per move — deterministic, hardware-independent. |
| `--movetime MS` | — | Fixed ms per move instead of nodes (mutually exclusive with `--nodes`). |
| `--maxplies M` | `200` | Safety cap on game length. |
| `-o`, `--out FILE` | `selfplay.html` | Output HTML path. |

Generated `*.html` files are git-ignored (see [.gitignore](../.gitignore)).

## Viewer controls

Auto-plays on load. Play/pause (Space), step (arrow keys or `◂`/`▸`), restart
(Home), a scrubber, and a speed toggle. Click any move in the list to jump. The
board highlights the last move and the checked king, and shows a check / checkmate
/ stalemate badge and an end-of-game banner. Check and mate/stalemate are computed
in-page by a compact attack detector (`attacked` in the embedded script), so the
result label is correct without a move generator.

## Functions

### `class Engine`

A single UCI engine subprocess driven line by line: `send` / `wait_for` (block for
a prefixed reply), `ready` (isready→readyok), `set_position` (apply a move list),
`bestmove` (issue a `go` and read `bestmove`), `fen` (reconstruct the current FEN
from the `d` dump), and `quit`.

### `Engine.fen`

Reconstruct the current position's FEN from the engine's `d` dump: parse the 8
board rows and the `Side to move` / `Castling rights` / `EP` / `Halfmoves` /
`Fullmoves` fields, then assemble the FEN string. Returns `None` if the dump did
not yield 8 rows.

### `_compress`

Collapse one rank of piece tokens (`['r','.','.','k', …]`) into its FEN
run-length form (`r2k…`).

### `record`

Play one self-play game to a ply cap. After each `bestmove`, apply the move and
dump the post-move FEN; terminate on no legal move (checkmate/stalemate), threefold
repetition, the fifty-move rule, or the ply cap. Prints a live `playing... N plies`
counter to stderr each ply so a long run never looks hung. Returns
`{"start", "moves":[{uci, fen}], "result"}`.

### `write_html`

Inject the recorded game into `HTML_TEMPLATE` (replacing the `__GAME_JSON__`
placeholder with `json.dumps(game)`) and write the standalone file.

### `main`

Parse args, choose the `go` limit (`--movetime` beats the default `--nodes`),
record the game, and write the HTML.

### `HTML_TEMPLATE`

The full standalone viewer document as a raw string with a `__GAME_JSON__`
placeholder — CSS, markup, and the vanilla-JS board/playback logic, all inline so
the emitted file has no external dependencies.

## Related

- Headless match/SPRT harness: [match.py.md](match.py.md).
- Build/run & the engine `d` command: [build.md](../build.md), [uci.hpp.md](../src/uci.hpp.md).
- Style guide (tooling companions): [documentation-style-guide.md §2.7](../documentation-style-guide.md#27-tooling-scripts).
