# Build & Toolchain

How ChessAIv2 is compiled. C++20, single self-contained binary, `-march=native` for
this machine (Ryzen 9 5900X, Zen 3), Clang-primary release builds. Two build paths
are kept in sync: **CMake** (primary, for IDEs) and a thin **Makefile** (for
OpenBench). The rationale behind the toolchain choices lives in
[PLAN.md](PLAN.md) §0.2–0.4 and §28; day-to-day build/run commands are in
[README.md](README.md).

The vendored library `include/chess.hpp` is header-only and self-initializing — there
is nothing to build for it.

## Files Covered

| File | Purpose |
|------|---------|
| [CMakeLists.txt](CMakeLists.txt) | Primary build. Broad IDE/tooling support (CLion, VS, VS Code); works with Clang, GCC, and MSVC. |
| [Makefile](Makefile) | Thin OpenBench-compatible build (`make EXE=<name>` then `<name> bench`). |

## Target

Both builds produce the engine executable, **`chessai`** (`chessai.exe` on Windows),
from `src/*.cpp`, with `include/` and `src/` on the include path. They also build two
optional **offline tuning tools** from `tools/` — `tuner` and `extract` (see
[Tuning tools](#tuning-tools)) — which are not part of the engine and are never linked
into it. CMake builds all targets by default; the Makefile builds `chessai` by default
and the tools via their named targets (`make tuner`, `make extract`).

## Compiler & Flags

| Concern | CMake ([CMakeLists.txt](CMakeLists.txt)) | Makefile ([Makefile](Makefile)) |
|---------|------------------------------------------|---------------------------------|
| C++ standard | `CMAKE_CXX_STANDARD 20`, extensions off | `-std=c++20` |
| Default compiler | toolchain default (any of Clang/GCC/MSVC) | `clang++` (`make CXX=g++` to switch) |
| Optimization (Release) | `-O3 -march=native -funroll-loops` (GCC/Clang) or `/O2 /arch:AVX2` (MSVC) | `-O3 -march=native -funroll-loops -flto -DNDEBUG` |
| LTO / IPO | auto-enabled when `check_ipo_supported` passes | `-flto` |
| PEXT movegen | `USE_PEXT` option (default ON) → `-DCHESS_USE_PEXT` | `-DCHESS_USE_PEXT` |
| Warnings | `-Wall -Wextra -Wno-unused-parameter` (GCC/Clang) | `-Wall -Wextra -Wno-unused-parameter` |
| Threads | `find_package(Threads)` → `Threads::Threads` | `-pthread` |

**`CHESS_USE_PEXT`** compiles the library's sliding move generation with BMI2 `pext`
— fast on Zen 3 / modern Intel (Haswell+). Turn it off on older AMD (microcoded,
slow `pext`), where the library falls back to magic bitboards. See
[PLAN.md](PLAN.md) §0.4 / §1.

**PGO** (profile-guided optimization, trained on a `bench` run — ~+5–10% NPS) is a
planned Phase-1 build-script step; not yet wired in.

## How to Build

Development targets **WSL2 (Ubuntu)** — Windows Smart App Control blocks running
unsigned self-built `.exe`s, and the Linux toolchain (clang/gcc + OpenBench,
fastchess, and later the `bullet` NNUE trainer) is the standard for engine work. The
canonical source lives on Windows and is synced into the Linux filesystem to build at
native speed (building on `/mnt/c` is slow). Full commands are in
[README.md](README.md); in brief:

### WSL2 / Ubuntu (canonical)

```bash
# rsync Windows source -> Linux fs, then build (Makefile path)
cd ~/ChessAIv2 && make               # -> ./chessai
```

### CMake (any platform / IDE)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build                  # -> build/chessai
```

### Native Windows / MSYS2

Compiles cleanly via CMake or `make`, but Smart App Control blocks *running* the
unsigned binary — hence WSL2 for actually executing it.

## How to Run

```
./chessai                -> UCI loop (default)
./chessai bench [depth]  -> deterministic benchmark (OpenBench signature)
./chessai perft test     -> move-generation correctness suite (exit 1 on failure)
```

See [main.cpp.md](src/main.cpp.md) for dispatch and [README.md](README.md) for the
full command list.

## Unit tests

Unit tests use **doctest** (single-header, vendored at `include/doctest.h` — third
party, treated like `chess.hpp` and not documented). The test binary
**`chessai-tests`** links every engine source *except* `src/main.cpp` (doctest
provides its own `main`) plus all of `tests/*.cpp`, so any module can be tested
without touching the runner ([tests/test_main.cpp.md](tests/test_main.cpp.md)).

```bash
make test            # Makefile: compile chessai-tests (fast -O1, no LTO) and run it
ctest --test-dir build --output-on-failure   # CMake: same binary via the `unit` test
```

Both build paths stay in sync (Makefile `test` target ↔ CMake `chessai-tests` target
+ `enable_testing`). The lighter `-O1` opt level keeps the loop fast; correctness and
node signatures are independent of it. `-Wno-\#warnings` silences a `<ciso646>`
`#warning` doctest triggers under a C++20 libstdc++. Test docs live next to the code
([tests/test_tt.cpp.md](tests/test_tt.cpp.md)) per
[documentation-style-guide.md §4.2](documentation-style-guide.md#42-test-files).

## Tuning tools

Two offline tools support the Phase 1c HCE eval-tuning loop
([PLAN.md](PLAN.md) §18). Neither is on the search path, so they may use heap and
I/O freely; both compile with the same C++20 / `-O3 -march=native` / PEXT flags as
the engine.

| Tool | Source | Links | Purpose |
|------|--------|-------|---------|
| `extract` | [tools/extract.cpp](tools/extract.cpp.md) | header-only (`chess.hpp`) | Replay a PGN and emit quiet, result-labeled positions (`FEN result` per line). |
| `tuner` | [tools/tune.cpp](tools/tune.cpp.md) | `src/eval.cpp` | Texel-tune the eval's piece-square tables to that dataset; print paste-ready C++ tables. |

```bash
make extract && make tuner                       # or CMake: builds both by default
./extract games/selfplay.pgn > dataset.txt       # PGN -> labeled FENs
./tuner dataset.txt --epochs 300 > tuned.txt      # fit tables; new PSQTs on stdout
```

The tuning workflow — generate self-play games (fastchess), `extract` a dataset,
`tuner` to fit the tables, paste the output into [src/eval.cpp](src/eval.hpp.md), then
SPRT-confirm — is described in [tools/tune.cpp.md](tools/tune.cpp.md). The tools are
excluded from the engine and test builds (the Makefile globs only `src/*.cpp`; the
tools have their own `main`).

## Commit gate

`git commit` is gated: [`.githooks/pre-commit`](.githooks/pre-commit) runs
[`scripts/run_checks.py`](scripts/run_checks.py.md), which must pass **build + unit
tests + perft + bench determinism + doc sync + C++ style** or the commit is blocked.
Activate it once per clone (it is a repo config, not committed):

```bash
git config core.hooksPath .githooks
```

Because the gate builds and runs the engine, commit from the WSL2 toolchain. Run it
by hand any time with `python3 scripts/run_checks.py`.

## Code Style & Formatting

C++ style is governed by [cpp-style-guide.md](cpp-style-guide.md). Its formatting half is
machine-enforced by [.clang-format](.clang-format) at the repo root (C++20, 4-space indent,
100 columns, attached braces, left-aligned pointers, preserved include grouping). Format any
file you touch before finishing:

```bash
clang-format -i src/<file>.hpp src/<file>.cpp    # or format-on-save in your editor
```

`.clang-format` is tuned to match the existing `src/` code, so running it is near-idempotent.
`clang-format` and `clang-tidy` are not in the base toolchain install ([README.md](README.md));
add them once in WSL with `sudo apt-get install -y clang-format clang-tidy`. The Python style
gate below needs no install.

A curated [.clang-tidy](.clang-tidy) adds advisory static analysis (bug-proneness, performance,
Core Guidelines); run it against a compile database:

```bash
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build src/*.cpp
```

The mechanical rules that clang-format cannot fix are gated by
[scripts/check_cpp_style.py](scripts/check_cpp_style.py) (documented in
[check_cpp_style.py.md](scripts/check_cpp_style.py.md)), wired into the Claude Code PostToolUse
and Stop hooks by [.claude/settings.json](.claude/settings.json) — it blocks a turn on tab
indentation, a header missing `#pragma once`, or `using namespace` in a header. Run it manually
with `python scripts/check_cpp_style.py --check`. Together these enforce layout, a slice of
semantics, and three mechanical rules; naming, single-responsibility, ownership, and hot-path
discipline are enforced by reading the guide (and its always-on rules in [CLAUDE.md](CLAUDE.md) /
[.agents/rules/cpp-style.md](.agents/rules/cpp-style.md)), not by any tool.

## Notes

- `-march=native` bakes in **this** CPU's ISA — the binary may `SIGILL` on older
  machines. Fine for a personal single-machine build; for distribution, build
  ISA-variant binaries (`x86-64-v2/v3/v4`) as [PLAN.md](PLAN.md) §0.4 describes.
- Keep the two build files in sync: a flag change in one should be mirrored in the
  other, and this document updated to match.
