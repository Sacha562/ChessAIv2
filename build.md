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

Both builds produce a single executable, **`chessai`** (`chessai.exe` on Windows),
from `src/*.cpp`, with `include/` and `src/` on the include path.

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

## Notes

- `-march=native` bakes in **this** CPU's ISA — the binary may `SIGILL` on older
  machines. Fine for a personal single-machine build; for distribution, build
  ISA-variant binaries (`x86-64-v2/v3/v4`) as [PLAN.md](PLAN.md) §0.4 describes.
- Keep the two build files in sync: a flag change in one should be mirrored in the
  other, and this document updated to match.
