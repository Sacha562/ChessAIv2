# Tooling: `check_cpp_style.py`

The C++ style checker. It backs the mechanical, zero-ambiguity half of
[cpp-style-guide.md](../cpp-style-guide.md) that [`.clang-format`](../.clang-format)
cannot (or should not) fix on its own, and gates it the same way
[`check_docs_sync.py`](check_docs_sync.py) gates documentation: a Claude Code
**Stop** hook that blocks the turn from ending while a blocking violation exists,
plus a **PostToolUse** nudge. It is deterministic, dependency-free (Python standard
library only), and **fails open** — a missing git or unreadable file never bricks a
session.

The checker is deliberately conservative. It **blocks only** on violations that are
(1) unambiguous, (2) explicit rules of the guide, and (3) *not* auto-fixed by
clang-format — so a block is always a real, must-fix problem. Everything softer is a
non-blocking advisory. Naming, magic numbers, and `std::endl` are **not** checked here
(the guide governs them by human judgement — see
[cpp-style-guide.md §16.5](../cpp-style-guide.md#165-what-the-tooling-cannot-check-you-must)
and the [`.clang-tidy`](../.clang-tidy) header).

This is developer tooling and never links into the engine binary; it lives under
`scripts/` (excluded from the C++ rules) but, being first-party code, carries this
companion per [documentation-style-guide.md §2.7](../documentation-style-guide.md#27-tooling-scripts).

## Source File

- **Code:** [check_cpp_style.py](check_cpp_style.py)

## How to Build & Run

Python 3.8+. No build, no dependencies. From the repo root:

```bash
python scripts/check_cpp_style.py --check          # working-tree report (exit 1 if a blocking violation exists)
python scripts/check_cpp_style.py --check --base origin/main   # committed changes vs. a base (CI)
python scripts/check_cpp_style.py --event stop      # Stop-hook JSON (may print a block decision)
python scripts/check_cpp_style.py --event post-tool # PostToolUse-hook JSON (reads tool payload on stdin)
```

## Checks

### Blocking (Stop hook; `--check` exit code)

| ID | Rule | Guide |
|----|------|-------|
| tab-indent | No line is indented with a tab (4 spaces only). | [§6](../cpp-style-guide.md#6-formatting--layout) |
| pragma-once | Every header (`.hpp`/`.h`/`.hxx`) contains `#pragma once`. | [§2](../cpp-style-guide.md#2-file--module-structure) |
| using-namespace-in-header | No header contains `using namespace`. | [§3](../cpp-style-guide.md#3-namespaces) |

### Soft (PostToolUse nudge only; never blocks)

| ID | Note |
|----|------|
| trailing-whitespace | Lines with trailing whitespace (clang-format removes these). |
| c-style-cast | Heuristic flag for a C-style cast to a primitive; prefer `static_cast<>`. |

Every PostToolUse nudge also reminds you to run `.clang-format` on the edited file.

## Configuration Constants

Keep the file-set constants in step with [`check_docs_sync.py`](check_docs_sync.py)
so both checkers agree on what "first-party C++" means.

| Constant | Purpose |
|----------|---------|
| `HEADER_EXTS` / `SOURCE_EXTS` / `CPP_EXTS` | C++ header/source extensions; the checked set. |
| `VENDORED_PREFIXES` | Path prefixes that are third-party and never checked (`include/`). |
| `EXCLUDED_SEGMENTS` | Path segments never checked (`.git`, `.claude`, `.agents`, `build`, …). |
| `EXCLUDED_BASENAMES` | Specific filenames never checked (`chess.hpp`). |
| `_TAB_INDENT_RE` / `_USING_NAMESPACE_RE` / `_C_CAST_RE` | Compiled patterns for the three regex-driven checks. |
| `INSTRUCTIONS` | The remediation text appended to a block/report. |

## Functions

### `_norm`

Normalize a path to forward slashes so Windows (`\`) and POSIX (`/`) paths compare equal.

### `_is_first_party_cpp`

Return `True` if a repo-relative path is a first-party C++ file the guide governs —
i.e. its extension is in `CPP_EXTS` and it is not vendored, excluded, or the
`chess.hpp` library header.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `rel` | `str` | Repo-relative path (any slash style). |

**Returns:** `bool`.

### `_read_lines`

Read a file into a list of lines (`utf-8`, replacement on decode errors). Returns
`None` on `OSError` so callers fail open.

### `_code_view`

Return each line with `//` and `/* */` comments blanked out, tracking block-comment
state across lines. Naive about string literals (acceptable — the checked tokens do
not meaningfully occur inside strings here). Prevents commented-out examples from
tripping the `using namespace` / cast checks.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `lines` | `list[str]` | Raw file lines. |

**Returns:** `list[str]` — comment-stripped lines, index-aligned with the input.

### `check_file`

Run every check against one file (read from disk) and return
`(blocking, soft)` message lists. A missing/unreadable file yields `([], [])`.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `rel` | `str` | Repo-relative path to a first-party C++ file. |

**Returns:** `tuple[list[str], list[str]]` — `(blocking, soft)` human-readable messages.

**Side Effects:** reads the file from disk.

### `_fmt_lines`

Format a list of line numbers for a message, capping the count and appending
`+N more` when truncated.

### `_git`

Run `git -C <repo> …` and return stdout; raises `subprocess.CalledProcessError` on
non-zero exit (callers catch it to fail open).

### `get_present_cpp`

Return the sorted repo-relative first-party C++ files that were added/modified.
Working-tree changes by default (parses `git status --porcelain=v1 -uall`); committed
`base...HEAD` changes when `base` is given (CI). Deleted files are skipped.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `base` | `str \| None` | Optional base ref for CI mode. |

**Returns:** `list[str]` — first-party C++ paths to check.

### `_read_stdin_json`

Parse the hook payload from stdin as JSON; returns `{}` on empty/invalid input.

### `run_stop`

**Stop** hook entry point. Scans changed C++ files; if any has a *blocking*
violation, prints a `{"decision": "block", "reason": …}` object so the turn cannot
end. Fails open (returns `0`, no block) when git is unavailable.

**Returns:** `int` — exit code (always `0`; the block is conveyed in JSON).

### `run_post_tool`

**PostToolUse** hook entry point. Reads the edited file from the tool payload on
stdin; for a first-party C++ file, prints an `additionalContext` nudge listing any
blocking + soft findings and a clang-format reminder. Never blocks; stays silent for
non-C++ files.

**Returns:** `int` — exit code (`0`).

### `run_check`

Human-readable CLI/CI report. With `base`, checks committed changes
(`base...HEAD`); without it, the working tree. Prints blocking and advisory findings
separately.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `base` | `str \| None` | Optional base ref for CI mode. |

**Returns:** `int` — `1` if any blocking violation exists, else `0`.

### `main`

Argument dispatch: `--event stop` → [`run_stop`](#run_stop), `--event post-tool` →
[`run_post_tool`](#run_post_tool), otherwise [`run_check`](#run_check) (honoring
`--base`).

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `argv` | `list[str]` | Arguments (without the program name). |

**Returns:** `int` — process exit code.

## Related

- Style guide: [cpp-style-guide.md](../cpp-style-guide.md) (esp. [§16 Enforcement](../cpp-style-guide.md#16-enforcement-how-this-is-kept-honest)).
- Formatting / lint configs: [`.clang-format`](../.clang-format), [`.clang-tidy`](../.clang-tidy).
- Hook wiring: [`.claude/settings.json`](../.claude/settings.json).
- Always-on rules: [`CLAUDE.md`](../CLAUDE.md), [`.agents/rules/cpp-style.md`](../.agents/rules/cpp-style.md).
- Sibling checker: [`check_docs_sync.py.md`](check_docs_sync.py.md).
