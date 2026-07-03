#!/usr/bin/env python3
"""Comprehensive commit gate for ChessAIv2.

Runs the full correctness + hygiene suite and exits non-zero if anything fails, so
the ``.githooks/pre-commit`` hook can block a commit (``git config core.hooksPath
.githooks`` wires it up). Steps, in order:

  1. build the engine (fast flags) — a broken build fails here
  2. build + run the doctest unit tests (``make test``)
  3. perft correctness suite (``chessai perft test``)
  4. bench determinism — two runs must share one node signature
  5. documentation sync (``check_docs_sync.py --check``)
  6. C++ style gate (``check_cpp_style.py --check``)

Standard library only. It targets the WSL / Linux toolchain the project builds with
(see build.md); if the compiler is absent it **blocks** with guidance rather than
passing silently, matching the "only commit if all checks pass" contract.

Run manually any time: ``python3 scripts/run_checks.py``.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GATE_EXE = "chessai-gate"
GATE_OPT = "-O1 -march=native -DNDEBUG"   # fast: correctness doesn't need -O3/-flto


def _run(cmd: list[str], capture: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(REPO), text=True, capture_output=capture)


def _bench_nodes() -> int | None:
    """Return the node signature from a `bench` run, or None on failure."""
    result = _run([f"./{GATE_EXE}", "bench"], capture=True)
    if result.returncode != 0:
        return None
    matches = re.findall(r"(\d+)\s+nodes", result.stdout)
    return int(matches[-1]) if matches else None


def main() -> int:
    if shutil.which("make") is None or shutil.which("clang++") is None:
        print(
            "gate: build toolchain (make / clang++) not found. Commit from the WSL "
            "environment where the engine builds (see build.md).",
            file=sys.stderr,
        )
        return 1

    failures: list[str] = []

    # 1. Engine build. Remove any stale binary first so a failed build can't leave a
    #    passing perft/bench behind.
    (REPO / GATE_EXE).unlink(missing_ok=True)
    print("[gate 1/6] building engine ...")
    built = _run(["make", f"EXE={GATE_EXE}", f"OPT={GATE_OPT}"]).returncode == 0
    if not built:
        failures.append("engine build")

    # 2. Unit tests (builds its own binary, then runs it).
    print("[gate 2/6] unit tests ...")
    if _run(["make", "test"]).returncode != 0:
        failures.append("unit tests")

    # 3-4. Perft + bench need the engine binary from step 1.
    if built:
        print("[gate 3/6] perft ...")
        if _run([f"./{GATE_EXE}", "perft", "test"]).returncode != 0:
            failures.append("perft")

        print("[gate 4/6] bench determinism ...")
        first, second = _bench_nodes(), _bench_nodes()
        if first is None or second is None or first != second:
            failures.append(f"bench determinism (got {first} vs {second})")
    else:
        failures.append("perft + bench (skipped: build failed)")

    # 5. Documentation sync.
    print("[gate 5/6] documentation sync ...")
    if _run([sys.executable, "scripts/check_docs_sync.py", "--check"]).returncode != 0:
        failures.append("documentation sync")

    # 6. C++ style.
    print("[gate 6/6] C++ style ...")
    if _run([sys.executable, "scripts/check_cpp_style.py", "--check"]).returncode != 0:
        failures.append("C++ style")

    print()
    if failures:
        print("GATE FAILED:")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("GATE PASSED: build, unit tests, perft, bench, docs, and style all green.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
