# Tooling: `check_docs_sync.py`

The documentation sync checker. It enforces the core rule of
[documentation-style-guide.md](../documentation-style-guide.md) — *every code
change requires a matching `.md` companion change* — by comparing the git working
tree (or a committed diff) against the placement rules in
[Section 2](../documentation-style-guide.md#2-file-placement-rules) of the guide.
It is deterministic and dependency-free (Python standard library only), so keeping
docs in sync never depends on an agent remembering to.

This is the C++ adaptation of the ChessCoach checker. The defining difference is
the **header-keyed module mapping** (see [`_cpp_companion`](#_cpp_companion)):
`eval.hpp` and `eval.cpp` share the single companion `eval.hpp.md`, so editing
*either* one requires that doc. It is wired into two Claude Code hooks by
[`.claude/settings.json`](../.claude/settings.json) and can also gate CI.

This script is developer tooling and never links into the engine binary; it lives
under `scripts/` (excluded from the C++ rules) but, being first-party code, carries
this companion per [Section 2.7](../documentation-style-guide.md#27-tooling-scripts).

## Source File

- **Code:** [check_docs_sync.py](check_docs_sync.py)

## How to Build & Run

Python 3.8+. No build, no dependencies. From the repo root:

```bash
python scripts/check_docs_sync.py --check          # working-tree report (exit 1 if out of sync)
python scripts/check_docs_sync.py --check --base origin/main   # committed changes vs. a base (CI)
python scripts/check_docs_sync.py --event stop      # Stop-hook JSON (reads git; may print a block decision)
python scripts/check_docs_sync.py --event post-tool # PostToolUse-hook JSON (reads tool payload on stdin)
```

## Configuration Constants

Module-level sets that encode the placement rules. Keep these in lockstep with
[Section 2](../documentation-style-guide.md#2-file-placement-rules) of the guide.

| Constant | Purpose |
|----------|---------|
| `HEADER_EXTS` | Header extensions (`.hpp .h .hxx`) — a header keys its module's doc. |
| `SOURCE_EXTS` | Source extensions (`.cpp .cc .cxx`) — keyed to a sibling header if one exists. |
| `CPP_EXTS` | `HEADER_EXTS ∪ SOURCE_EXTS`; the hard-rule C++ set. |
| `TOOLING_EXTS` | Non-C++ first-party tooling (`.py`) that gets a plain per-file companion. |
| `BUILD_BASENAMES` / `BUILD_EXTS` | Build files (`CMakeLists.txt`, `Makefile`, `*.cmake`) that soft-map to `build.md`. |
| `BUILD_DOC` | `"build.md"` — the single build/toolchain doc. |
| `VENDORED_PREFIXES` | Path prefixes that are third-party and never checked (`include/`). |
| `EXCLUDED_SEGMENTS` | Path segments that are never checked (`.git`, `.claude`, `.agents`, `build`, …). |
| `EXCLUDED_BASENAMES` | Specific filenames never checked (`chess.hpp`, the vendored library). |

## Functions

### `_norm`

Normalize a path to forward slashes. Trivial helper used everywhere so Windows
(`\`) and POSIX (`/`) paths compare equal.

### `_is_excluded`

Return `True` for any path the guide does not document: `.md` files, vendored
`include/` paths, excluded basenames, and any path under an excluded segment.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `rel` | `str` | Repo-relative path (any slash style). |

**Returns:** `bool` — `True` if the path is exempt from all companion rules.

### `_is_build`

Return `True` if the path is a build file (`CMakeLists.txt`, `Makefile`, or a
`*.cmake`) that soft-maps to [`build.md`](../build.md).

### `_cpp_companion`

Compute the companion `.md` path for a C++ file under the **header-keyed module
rule** — the heart of the C++ adaptation.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `rel` | `str` | Repo-relative path to a `.hpp`/`.cpp`/… file. |

**Returns:** `str` — the repo-relative companion path. A header returns its own
`+ .md` (`eval.hpp` → `eval.hpp.md`). A source returns its **sibling header's**
doc when that header exists on disk (`eval.cpp` → `eval.hpp.md`), otherwise its own
(`main.cpp` → `main.cpp.md`).

**Side Effects:** reads the filesystem (`Path.exists`) to detect the sibling header.

**Warnings:** if a header is deleted from disk, a source that keyed off it will
compute its own `.cpp.md` here instead — handled deliberately by the orphan logic
in [`find_violations`](#find_violations) via the header path.

### `_companion_has_live_code`

Return `True` if any existing code file still maps to a given companion. A header
doc stays alive while the header *or* any same-stem source exists; a source-only /
tooling doc stays alive while its single file exists. Used for orphan detection so
deleting one file of a two-file module does not falsely flag the shared doc.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `companion` | `str` | A repo-relative `.md` companion path. |

**Returns:** `bool` — `True` if live code still maps here (doc must stay).

**Side Effects:** reads the filesystem.

### `classify`

Central policy function: map a code path to its companion requirement.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `rel` | `str` | Repo-relative path. |

**Returns:** one of
- `("hard", "<companion>.md")` — companion must exist and be co-changed (C++ modules, tooling).
- `("soft", [candidates], label)` — at least one candidate doc must change (build files → `build.md`).
- `None` — the file is not documented by the guide.

### `_git`

Run `git -C <repo> …` and return stdout. Raises `subprocess.CalledProcessError`
on non-zero exit (callers catch it to fail open).

### `get_working_tree_changes`

Parse `git status --porcelain=v1 -uall` into `(present, deleted)` sets of
repo-relative paths. `present` = added/modified/renamed-to/untracked files that
exist on disk; `deleted` = removed files and rename sources.

**Returns:** `tuple[set[str], set[str]]` — `(present, deleted)`.

### `get_diff_changes`

Like [`get_working_tree_changes`](#get_working_tree_changes) but for committed
changes between a base and `HEAD` (three-dot `base...HEAD` range), for CI PR checks.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `base` | `str` | Base ref (e.g. `origin/main`). |

**Returns:** `tuple[set[str], set[str]]` — `(present, deleted)`.

### `find_violations`

The core check. For each changed code file, verify its companion is present (hard)
or that some candidate doc changed (soft); for each deleted code file, flag an
orphaned companion that no live code maps to.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `present` | `set[str]` | Paths that exist with new content. |
| `deleted` | `set[str]` | Paths that were removed. |

**Returns:** `list[str]` — human-readable violation messages; empty if in sync.

### `run_stop`

**Stop** hook entry point. Scans the working tree; if out of sync, prints a
`{"decision": "block", "reason": …}` JSON object so Claude Code cannot end the
turn. Fails open (returns `0`, no block) when git is unavailable.

**Returns:** `int` — process exit code (always `0`; the *block* is conveyed in JSON).

### `run_post_tool`

**PostToolUse** hook entry point. Reads the edited file from the tool payload on
stdin; if that file's companion has not been touched yet, prints an
`additionalContext` reminder (non-blocking nudge). Stays silent if the doc is
already being updated.

**Returns:** `int` — process exit code (`0`).

### `run_check`

Human-readable CLI/CI report. With `base`, checks committed changes
(`base...HEAD`); without it, checks the working tree.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `base` | `str \| None` | Optional base ref for CI mode. |

**Returns:** `int` — `0` if in sync, `1` if out of sync (so it can gate CI).

### `main`

Argument dispatch: `--event stop` → [`run_stop`](#run_stop), `--event post-tool`
→ [`run_post_tool`](#run_post_tool), otherwise [`run_check`](#run_check) (honoring
`--base`).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `argv` | `list[str]` | Arguments (without the program name). |

**Returns:** `int` — process exit code.

## Related

- Style guide: [documentation-style-guide.md](../documentation-style-guide.md) (esp. [Section 9](../documentation-style-guide.md#9-enforcement-automated)).
- Hook wiring: [`.claude/settings.json`](../.claude/settings.json).
- Always-on rules: [`CLAUDE.md`](../CLAUDE.md), [`.agents/rules/documentation.md`](../.agents/rules/documentation.md).
