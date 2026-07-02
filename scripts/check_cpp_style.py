#!/usr/bin/env python3
"""C++ style checker for ChessAIv2.

Backs the mechanical, zero-ambiguity half of ``cpp-style-guide.md`` that
``.clang-format`` cannot (or should not) enforce on its own. It is deliberately
conservative: it **blocks** a turn only on violations that are unambiguous, are
explicit rules of the guide, and that clang-format will *not* silently fix for you.
Everything softer is a non-blocking nudge. Like ``check_docs_sync.py`` it is
dependency-free (standard library only) and fails open, so it can never brick a
session.

Blocking checks (Stop hook / ``--check`` exit code):

* **tab-indent**    a line is indented with a tab (guide §6: 4 spaces, never tabs).
* **pragma-once**   a header (``.hpp``/``.h``/``.hxx``) is missing ``#pragma once``
                    (guide §2). clang-format will not add it.
* **using-namespace-in-header** a header contains ``using namespace`` (guide §3).
                    clang-format will not remove it.

Soft checks (PostToolUse nudge only, never block): trailing whitespace, and a
heuristic C-style-cast flag (guide §1). These are advisory because clang-format
handles the first and the second is a regex heuristic that can misfire.

Modes mirror ``check_docs_sync.py``:

* ``--event stop``      Stop hook. Scans changed first-party C++ files; prints a
                        ``{"decision": "block", ...}`` JSON object if any blocking
                        violation exists.
* ``--event post-tool`` PostToolUse hook. Reads the single edited file from the tool
                        payload on stdin and injects an ``additionalContext`` nudge
                        listing any blocking + soft findings (non-blocking).
* ``--check``           Human-readable report for CI / manual runs. Exits 1 when a
                        blocking violation exists, 0 otherwise.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# What counts as a first-party C++ file (kept in step with check_docs_sync.py).
# ---------------------------------------------------------------------------

HEADER_EXTS = {".hpp", ".h", ".hxx"}
SOURCE_EXTS = {".cpp", ".cc", ".cxx"}
CPP_EXTS = HEADER_EXTS | SOURCE_EXTS

VENDORED_PREFIXES = ("include/",)
EXCLUDED_SEGMENTS = {
    ".git", ".github", ".claude", ".agents", "plans",
    "build", "cmake-build-debug", "cmake-build-release", "out",
    "__pycache__", ".venv", "venv", "env",
    "scratchpad", ".idea", ".vscode",
}
EXCLUDED_BASENAMES = {"chess.hpp"}  # the vendored library header.

# A run of leading spaces/tabs that contains at least one tab.
_TAB_INDENT_RE = re.compile(r"^[ ]*\t")
_USING_NAMESPACE_RE = re.compile(r"\busing\s+namespace\b")
# Heuristic C-style cast to a known primitive/fixed-width type: (int)x, (uint64_t)y.
_C_CAST_RE = re.compile(
    r"\(\s*(?:const\s+)?(?:unsigned\s+|signed\s+)?"
    r"(?:void|bool|char|short|int|long|float|double|size_t|std::size_t|"
    r"u?int(?:8|16|32|64)_t)\s*\*?\s*\)\s*[A-Za-z0-9_(]"
)

INSTRUCTIONS = (
    "Fix these before finishing, then apply .clang-format, per cpp-style-guide.md "
    "(read it in full if you have not): tabs -> 4 spaces; every header starts with "
    "`#pragma once`; no `using namespace` in a header. If you believe a case is a "
    "genuine exception, ask the user - do not skip silently."
)


def _norm(rel: str) -> str:
    return rel.replace("\\", "/")


def _is_first_party_cpp(rel: str) -> bool:
    rel = _norm(rel)
    base = os.path.basename(rel)
    if base in EXCLUDED_BASENAMES:
        return False
    if any(rel.startswith(p) for p in VENDORED_PREFIXES):
        return False
    if any(seg in EXCLUDED_SEGMENTS for seg in rel.split("/")):
        return False
    _, ext = os.path.splitext(base)
    return ext in CPP_EXTS


def _read_lines(path: Path) -> list[str] | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None


def _code_view(lines: list[str]) -> list[str]:
    """Return each line with ``//`` and ``/* */`` comments blanked out.

    Naive (it does not track string literals), which is fine for the tokens we
    look for — ``using namespace`` and casts do not meaningfully occur inside
    strings in this codebase. Prevents commented-out examples from being flagged.
    """
    out: list[str] = []
    in_block = False
    for line in lines:
        result = []
        i = 0
        while i < len(line):
            two = line[i:i + 2]
            if in_block:
                if two == "*/":
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if two == "/*":
                in_block = True
                i += 2
                continue
            if two == "//":
                break  # rest of line is a comment
            result.append(line[i])
            i += 1
        out.append("".join(result))
    return out


def check_file(rel: str) -> tuple[list[str], list[str]]:
    """Return ``(blocking, soft)`` violation messages for one C++ file.

    ``rel`` is repo-relative; the file is read from disk. A missing/unreadable
    file yields no violations (fail open).
    """
    lines = _read_lines(REPO_ROOT / rel)
    if lines is None:
        return [], []

    _, ext = os.path.splitext(rel)
    is_header = ext in HEADER_EXTS
    code = _code_view(lines)

    blocking: list[str] = []
    soft: list[str] = []

    # --- Blocking ---------------------------------------------------------
    tab_lines = [i + 1 for i, ln in enumerate(lines) if _TAB_INDENT_RE.match(ln)]
    if tab_lines:
        blocking.append(f"{rel}: tab-indented lines (use 4 spaces): {_fmt_lines(tab_lines)}")

    if is_header:
        if not any(ln.strip() == "#pragma once" for ln in lines):
            blocking.append(f"{rel}: header is missing `#pragma once` (must be the first line).")
        un_lines = [i + 1 for i, ln in enumerate(code) if _USING_NAMESPACE_RE.search(ln)]
        if un_lines:
            blocking.append(
                f"{rel}: `using namespace` in a header (banned) on {_fmt_lines(un_lines)}."
            )

    # --- Soft -------------------------------------------------------------
    trailing = [i + 1 for i, ln in enumerate(lines) if ln != ln.rstrip()]
    if trailing:
        soft.append(f"{rel}: trailing whitespace on {_fmt_lines(trailing)} (clang-format fixes this).")

    cast_lines = [i + 1 for i, ln in enumerate(code) if _C_CAST_RE.search(ln)]
    if cast_lines:
        soft.append(
            f"{rel}: possible C-style cast on {_fmt_lines(cast_lines)} - "
            f"use static_cast<> (heuristic; ignore if false)."
        )

    return blocking, soft


def _fmt_lines(nums: list[str] | list[int], cap: int = 10) -> str:
    shown = ", ".join(str(n) for n in nums[:cap])
    return shown + (f", +{len(nums) - cap} more" if len(nums) > cap else "")


# ---------------------------------------------------------------------------
# Git plumbing.
# ---------------------------------------------------------------------------

def _git(args: list[str]) -> str:
    return subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args],
        capture_output=True, text=True, check=True,
    ).stdout


def get_present_cpp(base: str | None = None) -> list[str]:
    """Repo-relative first-party C++ files that were added/modified (exist on disk).

    Working-tree changes by default; committed ``base...HEAD`` changes when ``base``
    is given (CI). Deleted files are ignored — there is nothing left to lint.
    """
    present: set[str] = set()
    if base:
        out = _git(["diff", "--name-status", f"{base}...HEAD"])
        for line in out.splitlines():
            if not line.strip():
                continue
            parts = line.split("\t")
            code = parts[0]
            if code.startswith("D"):
                continue
            present.add(parts[-1])
    else:
        out = _git(["status", "--porcelain=v1", "-uall"])
        for line in out.splitlines():
            if not line.strip():
                continue
            status, rest = line[:2], line[3:]
            if " -> " in rest:  # rename / copy: keep the destination
                rest = rest.split(" -> ", 1)[1]
            elif "D" in status:
                continue
            present.add(rest.strip().strip('"'))
    return sorted(p for p in (_norm(x) for x in present) if _is_first_party_cpp(p))


# ---------------------------------------------------------------------------
# Hook / CLI entry points.
# ---------------------------------------------------------------------------

def _read_stdin_json() -> dict:
    try:
        raw = sys.stdin.read()
        return json.loads(raw) if raw.strip() else {}
    except (json.JSONDecodeError, ValueError):
        return {}


def run_stop() -> int:
    """Stop hook: block the turn while any changed C++ file has a blocking violation."""
    try:
        files = get_present_cpp()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return 0  # not a git repo / git missing — fail open.

    blocking: list[str] = []
    for rel in files:
        blocking.extend(check_file(rel)[0])

    if not blocking:
        return 0
    reason = (
        "C++ style violations must be resolved before ending the turn:\n\n"
        + "\n".join(f"  - {v}" for v in blocking)
        + "\n\n"
        + INSTRUCTIONS
    )
    print(json.dumps({"decision": "block", "reason": reason}))
    return 0


def run_post_tool() -> int:
    """PostToolUse hook: nudge (never block) right after editing a C++ file."""
    payload = _read_stdin_json()
    tool_input = payload.get("tool_input") or {}
    file_path = tool_input.get("file_path") or tool_input.get("filePath")
    if not file_path:
        return 0
    try:
        rel = _norm(os.path.relpath(file_path, REPO_ROOT))
    except ValueError:
        return 0
    if rel.startswith("..") or not _is_first_party_cpp(rel):
        return 0

    blocking, soft = check_file(rel)
    parts = [
        f"C++ STYLE REMINDER for `{rel}` (see cpp-style-guide.md); "
        f"apply .clang-format before finishing."
    ]
    if blocking:
        parts.append("MUST FIX: " + "; ".join(blocking))
    if soft:
        parts.append("Consider: " + "; ".join(soft))
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PostToolUse",
            "additionalContext": " ".join(parts),
        }
    }))
    return 0


def run_check(base: str | None = None) -> int:
    """CI / manual report. Exit 1 if any blocking violation exists, else 0."""
    try:
        files = get_present_cpp(base)
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f"check_cpp_style: could not read git changes: {exc}", file=sys.stderr)
        return 0

    blocking: list[str] = []
    soft: list[str] = []
    for rel in files:
        b, s = check_file(rel)
        blocking.extend(b)
        soft.extend(s)

    if not blocking and not soft:
        print("C++ style: no issues in changed files.")
        return 0
    if blocking:
        print("C++ style violations (blocking):\n")
        for v in blocking:
            print(f"  - {v}")
    if soft:
        print("\nC++ style notes (advisory):\n")
        for v in soft:
            print(f"  - {v}")
    if blocking:
        print("\n" + INSTRUCTIONS)
        return 1
    return 0


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
