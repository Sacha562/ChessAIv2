# ChessAIv2

A high-performance UCI chess engine in C++20, built on the
[Disservin `chess-library`](https://github.com/Disservin/chess-library) for
board state and legal move generation.

The full component design, options, and staged roadmap live in
[`PLAN.md`](PLAN.md). This README covers building and running the current build.

## Status — Phase 0 (skeleton) — ✅ verified

A legal, playable UCI engine with a minimal search. Verified in WSL2: the perft
suite passes all 6 positions (startpos depth 6 = 119,060,324, at ~400–500M nps),
clean `clang++` build, deterministic bench, and live UCI play confirmed.

- ✅ Library integration (`include/chess.hpp`, header-only, self-initializing)
- ✅ UCI protocol (`uci`, `isready`, `ucinewgame`, `position`, `go`, `stop`, `setoption`, `quit`), threaded search so `stop` / `go infinite` work
- ✅ Iterative deepening + fail-soft, full-window alpha-beta negamax
- ✅ Transposition table (bucketed, aged, lock-less XOR, multiply-shift), TT-move ordering; `Hash` (MB) UCI option, `info hashfull`
- ✅ Material evaluation (side-relative centipawns) with mate-aware scoring
- ✅ Soft/hard time management (`movetime`, `wtime/btime/winc/binc/movestogo`, `depth`, `nodes`, `infinite`); soft/hard budgets tunable via the `TimeSoftPermille` / `TimeHardPermille` UCI options
- ✅ `perft` (+ `perft test` correctness suite) and a deterministic `bench` command

Coming next (see `PLAN.md`): SEE, quiescence, the full move-ordering stack, PVS,
null-move/LMR/pruning, tuned HCE, then NNUE.

## Build & run (WSL2 / Ubuntu)

Development targets **WSL2 (Ubuntu)**: Windows Smart App Control blocks unsigned
self-built `.exe`s, and the Linux toolchain (clang/gcc, plus OpenBench, fastchess,
and the `bullet` NNUE trainer later) is the standard for engine work. The canonical
source lives on Windows; sync it into the Linux filesystem to build at native speed
(building on `/mnt/c` is slow — use `~/ChessAIv2`).

One-time toolchain install (from PowerShell):

```powershell
wsl -d Ubuntu -u root -e bash -c "apt-get update && apt-get install -y build-essential clang make cmake rsync"
```

Sync + build + run (from PowerShell):

```powershell
# sync Windows source -> Linux fs, then build with clang++
wsl -d Ubuntu -e bash -c "rsync -a --delete --exclude=chessai --exclude=chessai.exe --exclude=build/ --exclude=.git/ /mnt/c/Users/sivuc/projects/ChessAIv2/ ~/ChessAIv2/ && cd ~/ChessAIv2 && make"

# verify + benchmark
wsl -d Ubuntu -e bash -c "cd ~/ChessAIv2 && ./chessai perft test"   # move-gen correctness (exit 1 on failure)
wsl -d Ubuntu -e bash -c "cd ~/ChessAIv2 && ./chessai bench"        # deterministic node/nps signature
```

Notes:
- `-march=native` targets this machine (Ryzen 9 5900X, Zen 3: AVX2 + fast BMI2/PEXT).
- `-DCHESS_USE_PEXT` is on by default; default compiler is `clang++` (`make CXX=g++` to switch).
- A native Windows / MSYS2 build also compiles cleanly (CMake or `make`), but Smart App
  Control blocks *running* the unsigned binary — hence WSL2.

## Engine commands

```
# CLI
./chessai              # UCI mode (point a GUI / cutechess / fastchess at this binary)
./chessai bench [d]    # deterministic benchmark (OpenBench signature); optional depth
./chessai perft test   # move-generation correctness suite

# Interactive UCI
uci
position startpos moves e2e4 e7e5
go movetime 1000          # or: go depth N / go wtime .. btime ..
d                         # print the current board
perft 5                   # perft divide from the current position
quit
```

## Testing — engine vs engine

Different versions of the engine play each other through
[`scripts/match.py`](scripts/match.py), a wrapper around **fastchess** (the modern
cutechess-cli successor; the aligned plan is [`PLAN.md`](PLAN.md) §29). Run it in
WSL2, where `make` and `fastchess` live. A "version" is either a **git commit**
(built and cached as `bin/chessai-<sha>`) or the **same binary with a UCI option
toggled** — both are supported.

```bash
# Phase-0 smoke test: working tree vs the main baseline, 100 games, crash-scan
python scripts/match.py working main

# Phase-1+ SPRT: a feature commit vs the previous one, balanced opening suite
python scripts/match.py --mode sprt HEAD HEAD~1 --book books/UHO_4060_v2.epd

# A/B a single tunable via a UCI option, no rebuild between sides
python scripts/match.py --mode sprt HEAD HEAD --opt-a Hash=128 --opt-b Hash=16

# Just print the fastchess command (no build, no run)
python scripts/match.py --dry-run working main
```

The default `sanity` mode is the useful one *now* (Phase 0): it plays a short
match and flags crashes, disconnects, illegal moves, and time losses — real Elo
`sprt` tests only start mattering once a real search lands (Phase 1a). Provide
fastchess via `--fastchess`, `$FASTCHESS`, `./bin/fastchess`, or `PATH`
(<https://github.com/Disservin/fastchess>); drop a balanced opening suite in
`books/`. `bin/`, `books/`, and `games/` are git-ignored. Full flag reference:
[`scripts/match.py.md`](scripts/match.py.md). **OpenBench** (distributed SPRT) is
the later step once there's spare hardware — the `make EXE=` target it needs is
already in place.

### Watch a self-play game

For a *watchable* game rather than a headless match,
[`scripts/watch_selfplay.py`](scripts/watch_selfplay.py) has the engine play
itself and writes a **standalone HTML board** you open in a browser (auto-play,
step, scrub, check/mate detection — no server, no dependencies):

```bash
python scripts/watch_selfplay.py ./chessai --nodes 20000 -o /mnt/c/Users/you/game.html
```

Open the output file (write it under `/mnt/c/...` to double-click from Windows).
Generated `*.html` files are git-ignored. Flags:
[`scripts/watch_selfplay.py.md`](scripts/watch_selfplay.py.md).

## Layout

```
include/chess.hpp   the Disservin chess-library (vendored)
src/types.hpp       score conventions (centipawns, mate scoring)
src/eval.*          evaluation (Phase 0: material)
src/search.*        iterative deepening + alpha-beta negamax + TT probe/store
src/tt.*            transposition table (bucketed, aged, lock-less)
src/uci.*           UCI command loop (threaded search)
src/perft.*         perft + correctness suite
src/bench.*         deterministic benchmark
src/main.cpp        entry point / command dispatch
scripts/match.py    engine-vs-engine match / SPRT runner (fastchess wrapper)
scripts/watch_selfplay.py   self-play -> standalone HTML board viewer
```
