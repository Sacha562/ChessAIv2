# Tooling: `match.py`

The engine-vs-engine match runner. A thin, dependency-free wrapper around
**fastchess** (the modern [cutechess-cli](https://github.com/cutechess/cutechess)
successor) that lets two versions of the engine play each other and reports the
result. It is the practical front end for [PLAN.md](../PLAN.md) Component 29's
daily loop — `perft -> bench -> fastchess + SPRT` — and for the Phase-0 use of
matches as a crash / illegal-move regression gate before a real search exists.

This script is developer tooling and never links into the engine binary; it lives
under `scripts/` (excluded from the C++ rules) but, being first-party code,
carries this companion per
[documentation-style-guide.md §2.7](../documentation-style-guide.md#27-tooling-scripts).
Python 3.8+, standard library only.

## Source File

- **Code:** [match.py](match.py)

## What it does

Resolves two *engine versions*, builds them if needed, and hands them to
fastchess. Two independent version strategies (both chosen for this project):

- **Per-commit binaries (strategy A).** A git ref (`HEAD`, a branch, a tag, a
  sha) is built into `bin/chessai-<shortsha>` via a throwaway `git worktree` +
  `make EXE=<path>`, **cached by sha** so a baseline only builds once. `working`
  builds the current (possibly dirty) working tree into `bin/chessai-working`.
- **UCI-option toggles (strategy B).** `--opt-a` / `--opt-b` pass `setoption`
  overrides to one side, so a single binary can A/B a tunable parameter (e.g.
  `Hash`) without rebuilding. The engine already declares and safely ignores
  unknown UCI options (see [uci.hpp.md](../src/uci.hpp.md#enginehandlesetoption)).

Two modes:

- **`sanity`** (default) — a short fixed-length match with `-recover` that scans
  fastchess output for crashes, disconnects, illegal moves, and time losses.
  Meaningful *now*, in Phase 0: it proves a change did not break the engine even
  though there is no Elo to gain yet. Exit code is non-zero if any problem marker
  or a non-zero fastchess exit is seen.
- **`sprt`** — a full Sequential Probability Ratio Test with configurable bounds,
  time control, and opening suite. The Phase-1+ loop for accepting/rejecting a
  change on real game results.

Everything (`make`, `fastchess`) runs on the Linux/WSL2 toolchain per
[README.md](../README.md) and [build.md](../build.md); fastchess is not part of
the base toolchain install and must be provided (see *How to Build & Run*).

## How to Build & Run

No build. Run from the repo root **inside WSL2** (where `make`/`fastchess` live):

```bash
# Phase-0 smoke test: current working tree vs the main baseline, 100 games
python scripts/match.py working main

# SPRT: a feature commit vs the previous one, balanced opening suite
python scripts/match.py --mode sprt HEAD HEAD~1 --book books/UHO_4060_v2.epd

# A/B one binary via a UCI option (strategy B), no rebuild between sides
python scripts/match.py --mode sprt HEAD HEAD --opt-a Hash=128 --opt-b Hash=16

# Deterministic, hardware-independent regression match (fixed nodes)
python scripts/match.py working main --nodes 100000

# See the exact fastchess command without building or running anything
python scripts/match.py --dry-run --mode sprt HEAD HEAD~1
```

Provide fastchess in any of: `--fastchess <path>`, the `FASTCHESS` env var,
`./bin/fastchess`, or on `PATH` (falls back to `cutechess-cli`). Build it once in
WSL from <https://github.com/Disservin/fastchess>.

### Engine specs

| Spec | Meaning |
|------|---------|
| `working` | Build the current working tree → `bin/chessai-working` (always rebuilt). |
| `<gitref>` | Build that commit (branch/tag/sha) → `bin/chessai-<shortsha>`, cached. |
| `ref:<gitref>` | Same as above, explicit prefix. |
| `bin:<path>` | Use an already-built binary as-is. |

### Opening suite

`--book` points at a balanced opening suite (EPD or PGN); the default
`books/UHO_4060_v2.epd` is used automatically when present. A balanced suite cuts
draw rate and variance so results reflect engine strength, not opening luck —
strongly recommended for SPRT, optional for a sanity smoke test (falls back to
startpos with a warning). Books are **not** committed; fetch one into `books/`
(git-ignored). Note per [PLAN.md](../PLAN.md) §26 that this testing suite is
separate from the engine's in-game opening book.

## Key Options

| Flag | Default | Purpose |
|------|---------|---------|
| `--mode {sanity,sprt}` | `sanity` | Match type (crash-scan vs. SPRT). |
| `--tc` / `--nodes` / `--depth` | `--tc 10+0.1` | Per-engine limit (mutually exclusive; nodes/depth are deterministic). |
| `--hash` / `--threads` | `64` / `1` | `option.Hash` (MB) / `option.Threads` applied to both. |
| `--concurrency` | `cpus-1` | Parallel games. |
| `--rounds` / `--games` | `50`/`40000` · `2` | Round cap (mode-dependent) and games/round (2 = color-swapped `-repeat`). |
| `--elo0 --elo1 --alpha --beta --model` | `0 5 0.05 0.05 normalized` | SPRT bounds/risk/model. PLAN.md §29 suggests gainer `[0, 2]`. |
| `--opt-a` / `--opt-b` | — | Repeatable `NAME=VAL` UCI overrides per side (strategy B). |
| `--book` / `--pgnout` / `--bindir` | `books/UHO_4060_v2.epd` · `games/match.pgn` · `bin` | Suite, PGN output, binary cache dir. |
| `--fastchess` / `--rebuild` / `--dry-run` | — | fastchess path · force rebuild · print command and exit. |

## Functions

### `build_ref`

Build a git ref into `bin/chessai-<shortsha>`, cached by sha. Adds a detached
`git worktree`, runs `make EXE=<abs path>` inside it, and always removes the
worktree afterwards so the real working tree is never disturbed.

**Returns:** `Path` to the built (or cached) binary.

### `build_working`

Build the current working tree into `bin/chessai-working` (always rebuilt, since
the tree may be dirty and changing).

### `resolve_engine`

Parse an engine spec (`working` / `bin:<path>` / `[ref:]<gitref>`) into
`(binary_path, display_name)`, building via [`build_ref`](#build_ref) /
[`build_working`](#build_working) as needed.

### `ensure_book`

Resolve the opening suite path, or return `None` to play from startpos. A missing
*explicit* suite is a hard error for a real run (only a warning under
`--dry-run`); a missing default suite just warns and hints how to fetch one.

### `build_fastchess_cmd`

Assemble the full fastchess argument vector from the two resolved engines, the
opening suite, and parsed CLI args: per-engine `-engine cmd=… name=… proto=uci`
plus `option.*` overrides, the shared `-each` limit/`Hash`/`Threads`, openings,
`-games/-repeat/-concurrency/-recover`, the `-sprt` block in SPRT mode, and
`-pgnout`.

### `stream`

Run a command with `subprocess.Popen`, echoing its output live to stdout while
capturing it. Returns `(returncode, captured_text)`.

### `scan_problems`

Return the output lines that hit a `PROBLEM_MARKERS` substring (`illegal`,
`disconnect`, `loses on time`, `crash`, `terminated`, …) **and** no
`BENIGN_MARKERS` substring — the latter suppresses fastchess's own opening-book
config warnings so only real engine misbehavior trips the pass/fail gate.

### `report`

Print the verdict (clean vs. a list of suspicious lines) and return a process exit
code: `0` when clean, `1` when any problem marker or non-zero fastchess exit was
seen.

### `_find_fastchess`

Locate the fastchess binary from `--fastchess`, `$FASTCHESS`, `./bin/fastchess`,
or `PATH` (falling back to `cutechess-cli`); exit with guidance if none is found.

### `parse_args` / `main`

`parse_args` defines the CLI (see *Key Options*). `main` fills mode-dependent
round defaults, resolves both engines (disambiguating identical names with
`-a`/`-b` suffixes), resolves the suite and fastchess binary, then either prints
the command (`--dry-run`) or runs it via [`stream`](#stream) and
[`report`](#report).

## Related

- Testing methodology & SPRT theory: [PLAN.md](../PLAN.md) §29.
- Build/toolchain (`make EXE=`, WSL2): [build.md](../build.md), [README.md](../README.md).
- UCI options the harness toggles: [uci.hpp.md](../src/uci.hpp.md).
- Sibling tooling: [check_docs_sync.py.md](check_docs_sync.py.md),
  [check_cpp_style.py.md](check_cpp_style.py.md).
- Style guide (tooling companions): [documentation-style-guide.md §2.7](../documentation-style-guide.md#27-tooling-scripts).
