#!/usr/bin/env python3
"""Documentation sync checker for ChessAIv2.

Enforces the core rule of ``documentation-style-guide.md``: every C++ module has a
colocated ``.md`` companion, and whenever a module's code changes its companion must
change with it. The check is deterministic and driven purely by the git working
tree, so it never depends on an LLM remembering to update docs.

This is the C++ adaptation of the ChessCoach checker. The key difference is the
**header-keyed module mapping**: a header (``eval.hpp``) and its same-stem source
(``eval.cpp``) share **one** companion named after the header (``eval.hpp.md``).
Editing either file requires that one doc to change. Files with no header key off
their own name (``main.cpp`` -> ``main.cpp.md``); header-only files likewise
(``types.hpp`` -> ``types.hpp.md``). See ``check_docs_sync.py.md`` for full docs.

It runs in three modes:

* ``--event stop``      Used by the Claude Code **Stop** hook. Scans the whole
                        working tree and, if any code change lacks a matching
                        doc change, prints a ``{"decision": "block", ...}`` JSON
                        object that prevents Claude from ending its turn.
* ``--event post-tool`` Used by the Claude Code **PostToolUse** hook. Reads the
                        single edited file from the tool payload on stdin and, if
                        its companion has not been touched yet, injects a reminder
                        via ``additionalContext`` (non-blocking nudge).
* ``--check``           Human-readable report for CI or manual runs. Exits 1 when
                        anything is out of sync, 0 otherwise.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Configuration — what requires a companion, and what to ignore.
# ---------------------------------------------------------------------------

# C++ headers and sources. A header + same-stem source form ONE module whose
# companion is named after the header (see _cpp_companion).
HEADER_EXTS = {".hpp", ".h", ".hxx"}
SOURCE_EXTS = {".cpp", ".cc", ".cxx"}
CPP_EXTS = HEADER_EXTS | SOURCE_EXTS

# Non-C++ first-party tooling that gets a plain per-file ``<file>.md`` companion.
TOOLING_EXTS = {".py"}

# Build files map to the single root build.md (soft rule): editing one requires
# build.md to change, per style guide §2.5 / §4.3.
BUILD_BASENAMES = {"CMakeLists.txt", "Makefile"}
BUILD_EXTS = {".cmake"}
BUILD_DOC = "build.md"

# Anything under these path prefixes is vendored/third-party and never checked.
VENDORED_PREFIXES = ("include/",)

# Any path containing one of these segments is never checked.
EXCLUDED_SEGMENTS = {
    ".git", ".github", ".claude", ".agents", "plans",
    "build", "cmake-build-debug", "cmake-build-release", "out",
    "__pycache__", ".venv", "venv", "env",
    "scratchpad", ".idea", ".vscode",
}

# Generated / lock files that never need docs.
EXCLUDED_BASENAMES = {"chess.hpp"}  # belt-and-suspenders: vendored library header.


def _norm(rel: str) -> str:
    return rel.replace("\\", "/")


def _is_excluded(rel: str) -> bool:
    rel = _norm(rel)
    base = os.path.basename(rel)
    if base.endswith(".md"):
        return True
    if base in EXCLUDED_BASENAMES:
        return True
    if any(rel.startswith(p) for p in VENDORED_PREFIXES):
        return True
    parts = rel.split("/")
    return any(p in EXCLUDED_SEGMENTS for p in parts)


def _is_build(rel: str) -> bool:
    base = os.path.basename(rel)
    _, ext = os.path.splitext(base)
    return base in BUILD_BASENAMES or ext in BUILD_EXTS


def _cpp_companion(rel: str) -> str:
    """Companion path for a C++ file under the header-keyed module rule.

    * A header keys its own doc: ``eval.hpp`` -> ``eval.hpp.md``.
    * A source with a same-stem header keys the HEADER's doc:
      ``eval.cpp`` -> ``eval.hpp.md`` (when ``eval.hpp`` exists on disk).
    * A source with no header keys its own doc: ``main.cpp`` -> ``main.cpp.md``.
    """
    rel = _norm(rel)
    directory, base = os.path.split(rel)
    stem, ext = os.path.splitext(base)
    if ext in SOURCE_EXTS:
        for hext in HEADER_EXTS:
            sibling = f"{directory}/{stem}{hext}" if directory else f"{stem}{hext}"
            if (REPO_ROOT / sibling).exists():
                return sibling + ".md"
    return rel + ".md"


def _companion_has_live_code(companion: str) -> bool:
    """True if any existing code file still maps to ``companion``.

    Used for orphan detection: a header doc stays valid while the header OR any
    same-stem source still exists; a source-only / tooling doc stays valid while
    its single file exists.
    """
    companion = _norm(companion)
    if not companion.endswith(".md"):
        return False
    primary = companion[:-3]  # strip ".md" -> e.g. src/eval.hpp | main.cpp | x.py
    if (REPO_ROOT / primary).exists():
        return True
    directory, base = os.path.split(primary)
    stem, ext = os.path.splitext(base)
    if ext in HEADER_EXTS:
        for sext in SOURCE_EXTS:
            sibling = f"{directory}/{stem}{sext}" if directory else f"{stem}{sext}"
            if (REPO_ROOT / sibling).exists():
                return True
    return False


def classify(rel: str):
    """Return the companion requirement for a code path.

    * ``("hard", "<companion>.md")``     companion must exist and be co-changed.
    * ``("soft", [candidates], label)``  at least one candidate doc must change.
    * ``None``                            not something the style guide documents.
    """
    rel = _norm(rel)
    if _is_excluded(rel):
        return None

    base = os.path.basename(rel)
    _, ext = os.path.splitext(base)

    # Build files -> single build.md
    if _is_build(rel):
        return ("soft", [BUILD_DOC], BUILD_DOC)

    # C++ modules -> header-keyed companion
    if ext in CPP_EXTS:
        return ("hard", _cpp_companion(rel))

    # First-party tooling (Python, ...) -> plain per-file companion
    if ext in TOOLING_EXTS:
        return ("hard", rel + ".md")

    return None


# ---------------------------------------------------------------------------
# Git plumbing.
# ---------------------------------------------------------------------------

def _git(args: list[str]) -> str:
    return subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args],
        capture_output=True, text=True, check=True,
    ).stdout


def get_working_tree_changes():
    """Return ``(present, deleted)`` sets of repo-relative paths.

    ``present`` = files added/modified/renamed-to/untracked (they exist on disk
    with new content). ``deleted`` = files removed (incl. rename sources).
    """
    out = _git(["status", "--porcelain=v1", "-uall"])
    present: set[str] = set()
    deleted: set[str] = set()
    for line in out.splitlines():
        if not line.strip():
            continue
        status, rest = line[:2], line[3:]
        if " -> " in rest:  # rename / copy: "old -> new"
            old, new = rest.split(" -> ", 1)
            deleted.add(old.strip().strip('"'))
            present.add(new.strip().strip('"'))
            continue
        path = rest.strip().strip('"')
        if "D" in status:
            deleted.add(path)
        else:
            present.add(path)
    return present, deleted


def get_diff_changes(base: str):
    """Return ``(present, deleted)`` for changes between ``base`` and ``HEAD``.

    Used in CI, where a PR's changes are committed rather than sitting in the
    working tree. Three-dot range so only branch changes vs. the merge base count.
    """
    out = _git(["diff", "--name-status", f"{base}...HEAD"])
    present: set[str] = set()
    deleted: set[str] = set()
    for line in out.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        code = parts[0]
        if code.startswith(("R", "C")) and len(parts) >= 3:  # rename / copy
            if code.startswith("R"):
                deleted.add(parts[1])
            present.add(parts[2])
            continue
        path = parts[-1]
        if code.startswith("D"):
            deleted.add(path)
        else:
            present.add(path)
    return present, deleted


# ---------------------------------------------------------------------------
# Core check.
# ---------------------------------------------------------------------------

def find_violations(present: set[str], deleted: set[str]) -> list[str]:
    present = {_norm(p) for p in present}
    deleted = {_norm(p) for p in deleted}
    violations: list[str] = []

    for rel in sorted(present):
        result = classify(rel)
        if result is None:
            continue
        kind = result[0]
        if kind == "hard":
            companion = result[1]
            if companion in present:
                continue
            if not (REPO_ROOT / companion).exists():
                violations.append(
                    f"{rel} was changed but its companion `{companion}` does not exist - create it."
                )
            else:
                violations.append(
                    f"{rel} was changed but its companion `{companion}` was NOT updated."
                )
        elif kind == "soft":
            candidates, label = result[1], result[2]
            if any(c in present for c in candidates):
                continue
            violations.append(
                f"{rel} was changed but {label} was NOT updated to match."
            )

    # Orphaned companions: code deleted but its doc still lingers with no live code.
    for rel in sorted(deleted):
        result = classify(rel)
        if result is None or result[0] != "hard":
            continue
        companion = result[1]
        if (
            (REPO_ROOT / companion).exists()
            and companion not in deleted
            and not _companion_has_live_code(companion)
        ):
            violations.append(
                f"{rel} was deleted but its companion `{companion}` still exists - delete or update it."
            )

    return violations


# ---------------------------------------------------------------------------
# Hook / CLI entry points.
# ---------------------------------------------------------------------------

INSTRUCTIONS = (
    "Update the documentation to match your code changes before finishing, following "
    "documentation-style-guide.md (read it in full if you have not). Every code change "
    "requires a matching `.md` companion change. Remember a C++ header and its source "
    "share ONE header-keyed companion (eval.hpp + eval.cpp -> eval.hpp.md). If you "
    "believe a file should be exempt, ask the user - do not skip silently."
)


def _read_stdin_json() -> dict:
    try:
        raw = sys.stdin.read()
        return json.loads(raw) if raw.strip() else {}
    except (json.JSONDecodeError, ValueError):
        return {}


def run_stop() -> int:
    """Stop hook: block turn end while docs are out of sync."""
    try:
        present, deleted = get_working_tree_changes()
    except (subprocess.CalledProcessError, FileNotFoundError):
        # Not a git repo, or git unavailable — fail open, don't brick the session.
        return 0
    violations = find_violations(present, deleted)
    if not violations:
        return 0
    reason = (
        "Documentation is out of sync with your code changes. "
        "You must resolve these before ending the turn:\n\n"
        + "\n".join(f"  - {v}" for v in violations)
        + "\n\n"
        + INSTRUCTIONS
    )
    print(json.dumps({"decision": "block", "reason": reason}))
    return 0


def run_post_tool() -> int:
    """PostToolUse hook: nudge immediately after editing a code file."""
    payload = _read_stdin_json()
    tool_input = payload.get("tool_input") or {}
    file_path = tool_input.get("file_path") or tool_input.get("filePath")
    if not file_path:
        return 0
    try:
        rel = _norm(os.path.relpath(file_path, REPO_ROOT))
    except ValueError:
        return 0
    if rel.startswith(".."):
        return 0

    result = classify(rel)
    if result is None:
        return 0

    try:
        present, _ = get_working_tree_changes()
        present = {_norm(p) for p in present}
    except (subprocess.CalledProcessError, FileNotFoundError):
        present = set()

    kind = result[0]
    if kind == "hard":
        companion = result[1]
        if companion in present:
            return 0  # already updating the doc — stay quiet
        exists = (REPO_ROOT / companion).exists()
        verb = "update" if exists else "create"
        context = (
            f"DOCUMENTATION REMINDER: you just edited `{rel}`. Per documentation-style-guide.md "
            f"you must {verb} its companion `{companion}` to match before finishing this turn."
        )
    else:
        _, _, label = result
        context = (
            f"DOCUMENTATION REMINDER: you just edited `{rel}`. Per documentation-style-guide.md "
            f"you must update {label} to match before finishing this turn."
        )

    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PostToolUse",
            "additionalContext": context,
        }
    }))
    return 0


def run_check(base: str | None = None) -> int:
    """CI / manual mode: human-readable report.

    With ``base`` set, compares ``base...HEAD`` (committed changes, for CI PR
    checks). Without it, compares the working tree (local/manual use).
    """
    try:
        if base:
            present, deleted = get_diff_changes(base)
        else:
            present, deleted = get_working_tree_changes()
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f"check_docs_sync: could not read git changes: {exc}", file=sys.stderr)
        return 0
    violations = find_violations(present, deleted)
    if not violations:
        print("Documentation is in sync with code changes.")
        return 0
    print("Documentation is OUT OF SYNC with code changes:\n")
    for v in violations:
        print(f"  - {v}")
    print("\n" + INSTRUCTIONS)
    return 1


def main(argv: list[str]) -> int:
    if "--event" in argv:
        event = argv[argv.index("--event") + 1]
        if event == "stop":
            return run_stop()
        if event == "post-tool":
            return run_post_tool()

    base = None
    if "--base" in argv:
        base = argv[argv.index("--base") + 1]
    return run_check(base)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
