# Tooling: `run_checks.py`

The **commit gate**: one command that runs the full correctness + hygiene suite and
exits non-zero if anything fails. It is what the [`.githooks/pre-commit`](#the-git-hook)
hook invokes to enforce *"only commit if all checks pass"*, and it doubles as a
manual pre-push sanity check. Developer tooling — never linked into the engine —
under `scripts/`, carrying this companion per
[documentation-style-guide.md §2.7](../documentation-style-guide.md#27-tooling-scripts).
Python 3.8+, standard library only.

## Source File

- **Code:** [run_checks.py](run_checks.py)

## What it does

Runs six steps from the repo root and collects failures (it does not stop at the
first), then prints `GATE PASSED` / `GATE FAILED` and returns `0` / `1`:

| # | Step | Command | Catches |
|---|------|---------|---------|
| 1 | Engine build | `make EXE=chessai-gate OPT=-O1 …` | compile / link breakage |
| 2 | Unit tests | `make test` | a failing doctest case |
| 3 | Perft | `chessai-gate perft test` | move-generation regressions |
| 4 | Bench determinism | `chessai-gate bench` ×2 | nondeterminism in the search path |
| 5 | Doc sync | `check_docs_sync.py --check` | code changed without its `.md` companion |
| 6 | C++ style | `check_cpp_style.py --check` | tabs / missing `#pragma once` / `using namespace` in a header |

A fast `-O1` build (no `-O3`/`-flto`) keeps the gate quick; correctness and the node
signature are unaffected by the optimization level. If `make` / `clang++` are not on
`PATH` it **blocks with guidance** (commit from the WSL toolchain, see
[build.md](../build.md)) rather than passing silently.

## How to Build & Run

No build. From the repo root inside WSL2:

```bash
python3 scripts/run_checks.py     # runs all six steps; exit 0 iff all pass
```

### The git hook

[`.githooks/pre-commit`](../.githooks/pre-commit) is a thin wrapper that `exec`s this
script. Activate it once per clone:

```bash
git config core.hooksPath .githooks
```

After that, every `git commit` runs the gate and is blocked unless it passes. Because
the build/test steps need the compiler, commit from the WSL2 environment.

## Functions

### `main`

Orchestrate the six steps, guarding perft/bench on a successful build, and return the
process exit code (`0` clean, `1` on any failure or a missing toolchain).

### `_run`

Thin `subprocess.run` wrapper rooted at the repo, optionally capturing output.

### `_bench_nodes`

Run `chessai-gate bench` and parse the final `<n> nodes` signature (or `None` on
failure) for the determinism comparison.

## Related

- The checks it wraps: [check_docs_sync.py.md](check_docs_sync.py.md),
  [check_cpp_style.py.md](check_cpp_style.py.md).
- The tests it runs: [test_tt.cpp.md](../tests/test_tt.cpp.md),
  [build.md](../build.md) (the `test` target).
- Sibling tooling: [match.py.md](match.py.md).
