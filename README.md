# ChessAIv2

A high-performance UCI chess engine in C++20, built on the
[Disservin `chess-library`](https://github.com/Disservin/chess-library) for
board state and legal move generation. Releases are published as **SmikkelBot**.

The full component design and staged roadmap live in [`PLAN.md`](PLAN.md), whose
[Project status](PLAN.md#project-status) section tracks what shipped and what each
step was worth. This README covers what the engine does today and how to build,
run, and test it.

## Status — v0.3 (NNUE, net2) · next: faster inference, more data

| Release | Phase | Highlights |
|---|---|---|
| [v0.3](https://github.com/Sacha562/ChessAIv2/releases/tag/v0.3) | 2 — NNUE (net2) | Second net: `(768→1024)×2` SCReLU with 8 material output buckets, trained on 400M self-play positions (100M HCE + 300M net1 games): **+201.9 Elo** vs v0.2.1 |
| [v0.2.1](https://github.com/Sacha562/ChessAIv2/releases/tag/v0.2.1) | 2 — fix | Extension budget: check/singular extensions no longer stack and stop past ply 2×depth, so long checking lines (queen endgames) finish iterations and mate instead of shuffling (KQvK 0/8 → 7/8 mated); **+8.5 Elo** vs v0.2 |
| [v0.2](https://github.com/Sacha562/ChessAIv2/releases/tag/v0.2) | 2 — NNUE | First NNUE (`(768→512)×2` SCReLU, trained on 100M of its own self-play positions) embedded and made the default eval: **+252.5 Elo** vs the hand-crafted eval; `datagen` self-play generator |
| v0.1e | 3 — pondering | `go ponder` / `ponderhit` with full-PV `bestmove … ponder …` (**+131 Elo** vs ponder off), instant reply to a forced move |
| v0.1d | 1d — search selectivity | Correction history + history-modulated LMR (**+32.5 Elo** vs v0.1c), SPSA re-tune (**+6.1** at LTC) |
| v0.1c | 1c — hand-crafted eval | PSQT + tapered eval, mobility, pawn structure, piece terms, king safety, singular extensions |
| v0.1b | 1b — ordering & pruning | Killers/history/countermove, NMP, LMR, RFP, futility, LMP, aspiration (**+545** vs v0.1a) |
| v0.1a | 1a — search core | TT, SEE, quiescence, PVS, time management |

Elo figures come from self-play SPRT tests against the previous version; per-feature
numbers are in [PLAN.md](PLAN.md#project-status).

### What the engine does

- **Search:** iterative deepening, fail-soft PVS, aspiration windows, quiescence
  (captures, SEE pruning, delta pruning), check and singular extensions, IIR.
- **Selectivity:** null-move pruning, LMR (log table, history-adjusted), reverse
  futility, futility, late-move pruning, correction history on the static eval.
- **Move ordering:** TT move → good captures (SEE + MVV-LVA) → killers → countermove
  → butterfly + continuation history → bad captures.
- **Evaluation:** NNUE by default — a `(768 → 1024)×2` SCReLU network with 8 output
  buckets (picked by piece count) embedded in the binary, with incrementally updated
  accumulators (`EvalFile` loads another net, `UseNNUE false` switches back). The
  hand-crafted, tapered eval (PeSTO-seeded piece-square tables, mobility, pawn
  structure, bishop pair / rooks / outposts, king safety with pawn shield and storm)
  remains as the fallback.
- **Infrastructure:** bucketed lock-less transposition table, soft/hard time
  management (a forced move is answered at once), pondering (`go ponder` /
  `ponderhit`, full-PV `info` lines, `bestmove … ponder …`), deterministic `bench`,
  `perft` suite.
- **Not yet:** multi-threading (`Threads` is accepted but ignored), tablebases,
  MultiPV. See the roadmap in [PLAN.md](PLAN.md#part-8--recommended-roadmap-phased).

## Download

Prebuilt binaries are on the [releases page](https://github.com/Sacha562/ChessAIv2/releases):

| File | Use on |
|---|---|
| `chessai-<ver>-<os>-bmi2` | Intel Haswell+ / AMD Zen 3+ (fast PEXT) |
| `chessai-<ver>-<os>-avx2` | any AVX2 CPU, including AMD Zen 1/2 |

Each release lists its `bench` node count and a `SHA256SUMS` file. Point any UCI GUI
(Cute Chess, Arena, BanksiaGUI, En Croissant) at the binary.

## Build & run (WSL2 / Ubuntu)

Development targets **WSL2 (Ubuntu)**: Windows Smart App Control blocks unsigned
self-built `.exe`s, and the Linux toolchain (clang, fastchess, OpenBench) is the
standard for engine work. The canonical source lives on Windows; sync it into the
Linux filesystem to build at native speed (building on `/mnt/c` is slow).

One-time toolchain install (from PowerShell):

```powershell
wsl -d Ubuntu -u root -e bash -c "apt-get update && apt-get install -y build-essential clang clang-format make cmake rsync"
```

Sync + build (from PowerShell):

```powershell
wsl -d Ubuntu -e bash -c "rsync -a --delete --exclude=chessai --exclude=chessai.exe --exclude=build/ --exclude=.git/ --exclude=bin/ --exclude=books/ --exclude=games/ --exclude=dist/ /mnt/c/Users/sivuc/projects/ChessAIv2/ ~/ChessAIv2/ && cd ~/ChessAIv2 && make"
```

The `bin/`, `books/`, `games/`, and `dist/` excludes matter: those directories exist
only on the Linux side (test binaries, opening book, match logs), and `--delete`
would wipe them.

Verify:

```powershell
wsl -d Ubuntu -e bash -c "cd ~/ChessAIv2 && ./chessai perft test"   # move-gen correctness (exit 1 on failure)
wsl -d Ubuntu -e bash -c "cd ~/ChessAIv2 && ./chessai bench"        # deterministic node/nps signature
wsl -d Ubuntu -e bash -c "cd ~/ChessAIv2 && make test"              # doctest unit tests
```

Notes:
- Dev builds use `-march=native` with `-DCHESS_USE_PEXT`; default compiler is
  `clang++` (`make CXX=g++` to switch). Release builds use portable
  `-march=x86-64-v3` in `avx2` (magic) and `bmi2` (PEXT) variants — see
  [build.md](build.md).
- CMake works too (`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`).
- A native Windows / MSYS2 build compiles cleanly, but Smart App Control blocks
  *running* the unsigned binary.

### Commit gate

Commits are blocked unless **build + unit tests + perft + bench determinism + doc
sync + C++ style** all pass ([`scripts/run_checks.py`](scripts/run_checks.py.md)).
Enable it once per clone:

```bash
git config core.hooksPath .githooks
```

From Git Bash on Windows the hook re-runs the checks inside WSL automatically.

## Engine commands

```
# CLI
./chessai              # UCI mode (point a GUI / cutechess / fastchess at this binary)
./chessai bench [d]    # deterministic benchmark (OpenBench signature); optional depth
./chessai perft test   # move-generation correctness suite
./chessai datagen <out-prefix> <threads> <positions> [seed] [soft-nodes]   # NNUE training data

# Interactive UCI
uci
position startpos moves e2e4 e7e5
go movetime 1000          # or: go depth N / go nodes N / go wtime .. btime ..
d                         # print the current board
perft 5                   # perft divide from the current position
quit
```

### UCI options

| Group | Options |
|---|---|
| General | `Hash` (MB, default 16), `Threads` (accepted, not used yet), `Ponder` (default off; the GUI enables it) |
| Evaluation | `EvalFile` (default `<embedded>`: the network built into the binary; a path loads another bullet `quantised.bin`), `UseNNUE` (default true; false = hand-crafted eval) |
| Time | `TimeSoftPermille`, `TimeHardPermille`, `AssumedMovestogo`, `PonderCreditPermille` (share of ponder time not charged to the clock after a `ponderhit`, default 500) |
| Feature toggles (0/1) | `UseKillers`, `UseHistory`, `UseCountermove`, `UseContHist`, `UseIIR`, `UseNMP`, `UseRFP`, `UseFutility`, `UseLMP`, `UseLMR`, `UseCheckExt`, `UseSingular`, `UseAspiration`, `UseHistLmr`, `UseCorrHist` (all on); `UseHistPrune`, `UseCapHist`, `UseRazoring`, `UseProbCut` (off) |
| Tunable margins | `LmrBase`, `LmrDivisor`, `NmpBase`, `NmpEvalDiv`, `RfpMargin`, `FutMargin`, `FutBase`, `LmpBase`, `SingularMinDepth`, `SingularMargin`, `SingularDoubleMargin`, `HistPruneMargin`, `HistLmrDiv`, `RazorBase`, `RazorMargin`, `ProbCutMinDepth`, `ProbCutMargin`, `AspirationDelta`, `Tempo`, `DeltaMargin`, `EndgamePieces` |

The toggles and margins exist so features can be A/B-tested and SPSA-tuned without
rebuilding; the defaults are the tested values. Full reference:
[`src/uci.hpp.md`](src/uci.hpp.md).

## Testing — engine vs engine

Different versions play each other through [`scripts/match.py`](scripts/match.py),
a wrapper around **fastchess**. A "version" is either a **git commit** (built and
cached as `bin/chessai-<sha>`), a prebuilt binary (`bin:path`), or the **same binary
with UCI options toggled**. Run it in WSL2.

```bash
# SPRT: a feature commit vs the previous one, balanced opening suite
python3 scripts/match.py --mode sprt HEAD HEAD~1 --book books/UHO_4060_v2.epd

# A/B one feature on a single binary via a UCI toggle (no rebuild)
python3 scripts/match.py --mode sprt bin:bin/chessai-x bin:bin/chessai-x \
    --opt-a UseCorrHist=1 --opt-b UseCorrHist=0

# Smoke test: short match that flags crashes, illegal moves, time losses
python3 scripts/match.py working main

# Just print the fastchess command
python3 scripts/match.py --dry-run working main
```

Provide fastchess via `--fastchess`, `$FASTCHESS`, `./bin/fastchess`, or `PATH`
(<https://github.com/Disservin/fastchess>), and put a balanced opening suite
(e.g. `UHO_4060_v2.epd`) in `books/`. `bin/`, `books/`, and `games/` are
git-ignored. Full flag reference: [`scripts/match.py.md`](scripts/match.py.md).

The usual loop: STC 10+0.1 SPRT `[0, 5]` for a new feature; a fixed game count when
a result looks neutral; LTC 40+0.4 to confirm tuned parameters before baking them
into the defaults. Search parameters are SPSA-tuned on a self-hosted
**OpenBench** instance.

### Tuning tools

`tools/extract.cpp` turns PGNs into result-labeled positions; `tools/tune.cpp` is a
Texel tuner over the whole eval (with a check that its eval matches the engine's
exactly). Both build with `make extract` / `make tuner`; see [build.md](build.md).

### Watch a self-play game

[`scripts/watch_selfplay.py`](scripts/watch_selfplay.py) has the engine play itself
and writes a **standalone HTML board** you open in a browser (auto-play, step, scrub):

```bash
python3 scripts/watch_selfplay.py ./chessai --nodes 20000 -o /mnt/c/Users/you/game.html
```

Flags: [`scripts/watch_selfplay.py.md`](scripts/watch_selfplay.py.md).

## Opening book for lichess (`book/`)

SmikkelBot plays lichess through lichess-bot (Docker). The [`book/`](book/__init__.py.md)
service learns which openings work against **each opponent at each time control**,
falling back to results against all other opponents at that speed for newcomers.
It generates candidate moves with Stockfish, replays won games against bots that
repeat themselves, and has a local dashboard to inspect opponents, explore
positions, and add candidate moves by hand. It runs as a `book` service (with Stockfish 19) in the same Docker Compose stack
as lichess-bot (`LichessBotAPI/`), which builds it from this checkout:

```bash
docker compose up -d --build      # from LichessBotAPI/; dashboard at http://127.0.0.1:8765
```

Setup details (`homemade.py` shim, `config.yml`, local development):
[book/__init__.py.md](book/__init__.py.md#how-to-build--run).

## Layout

Every module has a colocated `.md` companion describing what it does (e.g.
`src/search.hpp.md`); read those first.

```
include/chess.hpp       Disservin chess-library (vendored)
include/doctest.h       doctest unit-test framework (vendored)
src/types.hpp           score conventions (centipawns, mate scoring)
src/search.*            iterative deepening, PVS, quiescence, pruning, Tunables
src/movepick.*          staged move ordering
src/history.*           killers, butterfly / continuation / capture history, countermoves
src/corrhist.*          correction history for the static eval
src/see.*               static exchange evaluation
src/nnue.*              NNUE evaluation: embedded net, incremental accumulators
src/eval.*              hand-crafted tapered evaluation (EvalParams), the fallback
src/datagen.*           self-play training-data generator (chessai datagen)
src/viriformat.*        viriformat game encoder for the bullet trainer
nets/net2.bin           default NNUE network, embedded at build time
src/tt.*                transposition table (bucketed, aged, lock-less)
src/uci.*               UCI command loop and options
src/perft.*             perft + correctness suite
src/bench.*             deterministic benchmark
src/main.cpp            entry point / command dispatch
tests/                  doctest unit tests (make test)
tools/extract.cpp       PGN -> labeled positions
tools/tune.cpp          Texel tuner
scripts/match.py        engine-vs-engine match / SPRT runner (fastchess wrapper)
scripts/run_checks.py   commit gate (build, tests, perft, bench, docs, style)
scripts/watch_selfplay.py   self-play -> standalone HTML board viewer
book/                   opponent-aware lichess opening book + dashboard (Python)
```
