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
- ✅ Iterative deepening + full-window alpha-beta negamax
- ✅ Material evaluation (side-relative centipawns) with mate-aware scoring
- ✅ Minimal time management (`movetime`, `wtime/btime/winc/binc/movestogo`, `depth`, `nodes`, `infinite`)
- ✅ `perft` (+ `perft test` correctness suite) and a deterministic `bench` command

Coming next (see `PLAN.md`): transposition table, quiescence + SEE, PVS,
move ordering, null-move/LMR/pruning, tuned HCE, then NNUE.

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

## Layout

```
include/chess.hpp   the Disservin chess-library (vendored)
src/types.hpp       score conventions (centipawns, mate scoring)
src/eval.*          evaluation (Phase 0: material)
src/search.*        iterative deepening + alpha-beta negamax
src/uci.*           UCI command loop (threaded search)
src/perft.*         perft + correctness suite
src/bench.*         deterministic benchmark
src/main.cpp        entry point / command dispatch
```
