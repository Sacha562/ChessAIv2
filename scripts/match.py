#!/usr/bin/env python3
"""Engine-vs-engine match runner for ChessAIv2.

A thin, dependency-free wrapper around **fastchess** (the modern cutechess-cli
successor) that lets two versions of the engine play each other and reports the
result. It is the practical front end for PLAN.md Component 29's daily loop —
``perft -> bench -> fastchess + SPRT`` — and for the Phase-0 use of matches as a
crash/illegal-move regression gate before a real search exists.

It resolves two *engine versions* and hands them to fastchess:

* **Per-commit binaries (strategy A).** A git ref (``HEAD``, a branch, a tag, a
  sha) is built into ``bin/chessai-<shortsha>`` via a throwaway ``git worktree``
  and ``make EXE=<path>``, cached by sha so a base only builds once. ``working``
  builds the current (possibly dirty) working tree into ``bin/chessai-working``.
* **UCI-option toggles (strategy B).** ``--opt-a``/``--opt-b`` pass
  ``setoption`` overrides to one side, so a single binary can A/B a tunable
  parameter without rebuilding.

Two modes:

* ``sanity`` (default) — a short fixed-length match with ``-recover`` that scans
  fastchess output for crashes, disconnects, illegal moves, and time losses.
  Meaningful *now*, in Phase 0: it proves a change did not break the engine even
  though there is no Elo to gain yet.
* ``sprt`` — a full Sequential Probability Ratio Test with configurable bounds,
  time control, and opening suite. The Phase-1+ loop for accepting/rejecting a
  change on real game results.

Everything (make, fastchess) runs on the Linux/WSL2 toolchain per
[README.md](../README.md); this script is Python 3.8+, standard library only.
See [match.py.md](match.py.md) for full documentation.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Defaults — every one is overridable on the command line.
# ---------------------------------------------------------------------------

DEFAULT_BINDIR = "bin"                       # where built version binaries land
DEFAULT_BOOK = "books/UHO_4060_v2.epd"       # balanced opening suite (see ensure_book)
DEFAULT_TC = "10+0.1"                         # STC: 10s + 0.1s/move
DEFAULT_HASH_MB = 64                          # keep small at STC
DEFAULT_THREADS = 1                           # single-threaded until Lazy SMP (Phase 1d)
DEFAULT_CONCURRENCY = max(1, (os.cpu_count() or 2) - 1)
DEFAULT_SANITY_ROUNDS = 50                    # x2 games/round = 100 games
DEFAULT_SPRT_ROUNDS = 40000                   # a cap; SPRT stops on its own

# SPRT bound defaults. PLAN.md §29 suggests gainer [0, 2]; Fishtest commonly uses
# [0, 5] at STC. Both are fine — override with --elo0/--elo1 per test.
DEFAULT_ELO0 = 0.0
DEFAULT_ELO1 = 5.0
DEFAULT_ALPHA = 0.05
DEFAULT_BETA = 0.05
DEFAULT_MODEL = "normalized"                  # nElo: bounds invariant to draw rate/TC

# Substrings (case-insensitive) in fastchess output that flag a broken engine.
# Kept tight so fastchess's own config chatter (e.g. "no opening book") is not
# mistaken for an engine failure — see BENIGN_MARKERS.
PROBLEM_MARKERS = (
    "illegal",
    "disconnect",
    "loses on time",
    "time forfeit",
    "stall",
    "crash",
    "terminated",
    "segmentation",
    "assert",
)

# Substrings that make a line benign even if it trips a PROBLEM_MARKER — fastchess
# configuration warnings, not engine misbehavior.
BENIGN_MARKERS = (
    "opening book",
    "opening format",
    "consider using",
)


# ---------------------------------------------------------------------------
# Small process helpers.
# ---------------------------------------------------------------------------

def _run(cmd: list[str], cwd: Path | None = None) -> str:
    """Run a command, return stdout, raise on non-zero exit."""
    result = subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, check=True
    )
    return result.stdout


def _git(args: list[str]) -> str:
    return _run(["git", "-C", str(REPO_ROOT), *args])


def _log(msg: str) -> None:
    print(f"[match] {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Building engine versions.
# ---------------------------------------------------------------------------

def _make(exe: Path, cwd: Path) -> None:
    """Invoke the Makefile's ``EXE=`` target to build ``exe`` from ``cwd``."""
    exe.parent.mkdir(parents=True, exist_ok=True)
    _log(f"building {exe.name} (make EXE={exe})")
    subprocess.run(["make", f"EXE={exe.resolve()}"], cwd=cwd, check=True)


def build_ref(ref: str, bindir: Path, rebuild: bool, dry_run: bool) -> Path:
    """Build a git ref into ``bindir/chessai-<shortsha>`` (cached by sha).

    Uses a detached ``git worktree`` so the build is hermetic and never disturbs
    the working tree. The worktree is always removed afterwards.
    """
    sha = _git(["rev-parse", "--short", ref]).strip()
    out = bindir / f"chessai-{sha}"
    if dry_run:
        return out
    if out.exists() and not rebuild:
        _log(f"reusing cached {out.name} (ref {ref} -> {sha})")
        return out

    worktree = bindir / ".worktrees" / sha
    if worktree.exists():
        _git(["worktree", "remove", "--force", str(worktree)])
    _git(["worktree", "add", "--detach", "--force", str(worktree), sha])
    try:
        _make(out, cwd=worktree)
    finally:
        _git(["worktree", "remove", "--force", str(worktree)])
    return out


def build_working(bindir: Path, dry_run: bool) -> Path:
    """Build the current working tree into ``bindir/chessai-working`` (always)."""
    out = bindir / "chessai-working"
    if dry_run:
        return out
    _make(out, cwd=REPO_ROOT)
    return out


def resolve_engine(spec: str, bindir: Path, rebuild: bool, dry_run: bool) -> tuple[Path, str]:
    """Resolve an engine spec to ``(binary_path, display_name)``.

    Spec grammar:

    * ``working``            build the current working tree.
    * ``bin:<path>``         use an already-built binary as-is.
    * ``ref:<gitref>`` /     build that commit (``ref:`` prefix optional — a bare
      ``<gitref>``           token is treated as a git ref).
    """
    if spec == "working":
        path = build_working(bindir, dry_run)
        return path, "working"
    if spec.startswith("bin:"):
        path = Path(spec[len("bin:"):]).expanduser()
        if not dry_run and not path.exists():
            sys.exit(f"[match] engine binary not found: {path}")
        return path, path.name
    ref = spec[len("ref:"):] if spec.startswith("ref:") else spec
    path = build_ref(ref, bindir, rebuild, dry_run)
    return path, path.name


# ---------------------------------------------------------------------------
# Opening suite.
# ---------------------------------------------------------------------------

def ensure_book(book: str | None, mode: str, dry_run: bool) -> Path | None:
    """Resolve the opening suite, or return ``None`` to play from startpos.

    A balanced suite (e.g. UHO_4060) cuts draw rate and variance so results
    reflect engine strength, not opening luck. It is strongly recommended for
    SPRT; optional for a smoke-test sanity match. A missing *explicit* suite is a
    hard error for a real run, but only a warning under ``--dry-run`` (there we
    just want to inspect the command).
    """
    if book is None:
        default = REPO_ROOT / DEFAULT_BOOK
        if default.exists():
            return default
        _log(f"no opening suite at {DEFAULT_BOOK}; playing from startpos.")
        if mode == "sprt":
            _log("  SPRT from startpos is high-variance — a balanced suite is recommended.")
        _log("  Fetch one, e.g.: "
             "https://github.com/official-stockfish/books (UHO_4060_v2.epd.zip) -> books/")
        return None

    path = (REPO_ROOT / book) if not os.path.isabs(book) else Path(book)
    if not path.exists():
        if dry_run:
            _log(f"opening suite not found (dry-run, ignoring): {path}")
            return path
        sys.exit(f"[match] opening suite not found: {path}")
    return path


def _book_format(path: Path) -> str:
    return "epd" if path.suffix.lower() == ".epd" else "pgn"


# ---------------------------------------------------------------------------
# fastchess command assembly.
# ---------------------------------------------------------------------------

def _engine_args(cmd: Path, name: str, opts: list[str]) -> list[str]:
    args = ["-engine", f"cmd={cmd}", f"name={name}", "proto=uci"]
    args += [f"option.{opt}" for opt in opts]
    return args


def _limit_args(args: argparse.Namespace) -> list[str]:
    """The per-engine search limit: nodes/depth (deterministic) override tc."""
    if args.nodes is not None:
        return [f"nodes={args.nodes}"]
    if args.depth is not None:
        return [f"depth={args.depth}"]
    return [f"tc={args.tc}"]


def build_fastchess_cmd(
    fastchess: str,
    a: tuple[Path, str],
    b: tuple[Path, str],
    book: Path | None,
    args: argparse.Namespace,
) -> list[str]:
    cmd: list[str] = [fastchess]
    cmd += _engine_args(a[0], a[1], args.opt_a)
    cmd += _engine_args(b[0], b[1], args.opt_b)
    cmd += ["-each", *_limit_args(args),
            f"option.Hash={args.hash}", f"option.Threads={args.threads}"]
    if book is not None:
        cmd += ["-openings", f"file={book}", f"format={_book_format(book)}", "order=random"]
    cmd += ["-games", str(args.games), "-repeat",
            "-concurrency", str(args.concurrency), "-recover"]
    if args.mode == "sprt":
        cmd += ["-rounds", str(args.rounds),
                "-sprt", f"elo0={args.elo0}", f"elo1={args.elo1}",
                f"alpha={args.alpha}", f"beta={args.beta}", f"model={args.model}"]
    else:
        cmd += ["-rounds", str(args.rounds)]
    cmd += ["-pgnout", f"file={args.pgnout}"]
    return cmd


# ---------------------------------------------------------------------------
# Running & result scanning.
# ---------------------------------------------------------------------------

def stream(cmd: list[str]) -> tuple[int, str]:
    """Run ``cmd``, echoing output live while capturing it. Returns (rc, text)."""
    proc = subprocess.Popen(
        cmd, cwd=REPO_ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    lines: list[str] = []
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        lines.append(line)
    proc.wait()
    return proc.returncode, "".join(lines)


def scan_problems(text: str) -> list[str]:
    """Return output lines that look like an engine misbehaving.

    A line flags only if it hits a `PROBLEM_MARKER` and no `BENIGN_MARKER` (so
    fastchess's opening-book config warnings do not read as engine failures).
    """
    hits = []
    for line in text.splitlines():
        low = line.lower()
        if any(m in low for m in PROBLEM_MARKERS) and not any(b in low for b in BENIGN_MARKERS):
            hits.append(line.strip())
    return hits


def report(mode: str, rc: int, text: str) -> int:
    """Print a verdict and return a process exit code (0 = clean)."""
    problems = scan_problems(text)
    print("\n" + "=" * 60)
    if problems:
        print(f"[match] {len(problems)} suspicious line(s) in {mode} match:")
        for line in problems[:20]:
            print(f"  ! {line}")
        if len(problems) > 20:
            print(f"  ... and {len(problems) - 20} more")
    else:
        print(f"[match] {mode} match completed with no crash/illegal/time-loss markers.")
    if rc != 0:
        print(f"[match] fastchess exited with code {rc}.")
    print("=" * 60)
    return 1 if (problems or rc != 0) else 0


# ---------------------------------------------------------------------------
# CLI.
# ---------------------------------------------------------------------------

def _find_fastchess(explicit: str | None, bindir: Path) -> str:
    for candidate in (explicit, os.environ.get("FASTCHESS"), str(bindir / "fastchess")):
        if candidate and (Path(candidate).exists() or shutil.which(candidate)):
            return candidate
    found = shutil.which("fastchess") or shutil.which("cutechess-cli")
    if found:
        return found
    sys.exit("[match] fastchess not found. Install it, put it on PATH or in ./bin, "
             "or pass --fastchess <path>. See https://github.com/Disservin/fastchess")


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="match.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Play two ChessAIv2 versions against each other via fastchess.",
        epilog=(
            "engine specs:\n"
            "  working            build the current working tree\n"
            "  <gitref>           build that commit (branch/tag/sha), cached by sha\n"
            "  ref:<gitref>       same, explicit prefix\n"
            "  bin:<path>         use an already-built binary\n\n"
            "examples:\n"
            "  # Phase-0 smoke test: this branch vs its merge-base, 100 games\n"
            "  python scripts/match.py working main\n\n"
            "  # SPRT: a feature commit vs the baseline, balanced suite\n"
            "  python scripts/match.py --mode sprt HEAD HEAD~1 --book books/UHO_4060_v2.epd\n\n"
            "  # A/B one binary via a UCI option (strategy B)\n"
            "  python scripts/match.py --mode sprt HEAD HEAD --opt-a Hash=128 --opt-b Hash=16\n"
        ),
    )
    p.add_argument("a", help="engine version A (see engine specs below)")
    p.add_argument("b", help="engine version B")
    p.add_argument("--mode", choices=("sanity", "sprt"), default="sanity",
                   help="sanity = fixed match + crash scan (default); sprt = full SPRT")

    limit = p.add_mutually_exclusive_group()
    limit.add_argument("--tc", default=DEFAULT_TC, help=f"time control (default {DEFAULT_TC})")
    limit.add_argument("--nodes", type=int, help="fixed nodes/move (deterministic, hw-independent)")
    limit.add_argument("--depth", type=int, help="fixed depth/move (deterministic)")

    p.add_argument("--hash", type=int, default=DEFAULT_HASH_MB, help=f"Hash MB (default {DEFAULT_HASH_MB})")
    p.add_argument("--threads", type=int, default=DEFAULT_THREADS, help=f"Threads (default {DEFAULT_THREADS})")
    p.add_argument("--concurrency", type=int, default=DEFAULT_CONCURRENCY,
                   help=f"parallel games (default {DEFAULT_CONCURRENCY})")
    p.add_argument("--rounds", type=int, default=None,
                   help="round cap (default 50 sanity / 40000 sprt)")
    p.add_argument("--games", type=int, choices=(1, 2), default=2,
                   help="games per round; 2 (+ -repeat) swaps colors on each opening")

    p.add_argument("--elo0", type=float, default=DEFAULT_ELO0, help="SPRT H0 bound")
    p.add_argument("--elo1", type=float, default=DEFAULT_ELO1, help="SPRT H1 bound")
    p.add_argument("--alpha", type=float, default=DEFAULT_ALPHA, help="SPRT alpha")
    p.add_argument("--beta", type=float, default=DEFAULT_BETA, help="SPRT beta")
    p.add_argument("--model", default=DEFAULT_MODEL, help="SPRT model (normalized|logistic)")

    p.add_argument("--opt-a", action="append", default=[], metavar="NAME=VAL",
                   help="UCI setoption override for engine A (repeatable)")
    p.add_argument("--opt-b", action="append", default=[], metavar="NAME=VAL",
                   help="UCI setoption override for engine B (repeatable)")

    p.add_argument("--book", default=None, help=f"opening suite (default {DEFAULT_BOOK} if present)")
    p.add_argument("--pgnout", default="games/match.pgn", help="PGN output path")
    p.add_argument("--bindir", default=DEFAULT_BINDIR, help=f"built-binary dir (default {DEFAULT_BINDIR})")
    p.add_argument("--fastchess", default=None, help="path to the fastchess binary")
    p.add_argument("--rebuild", action="store_true", help="force rebuild even if a cached binary exists")
    p.add_argument("--dry-run", action="store_true", help="print the fastchess command and exit")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.rounds is None:
        args.rounds = DEFAULT_SANITY_ROUNDS if args.mode == "sanity" else DEFAULT_SPRT_ROUNDS

    bindir = REPO_ROOT / args.bindir
    a = resolve_engine(args.a, bindir, args.rebuild, args.dry_run)
    b = resolve_engine(args.b, bindir, args.rebuild, args.dry_run)
    if a[1] == b[1]:  # fastchess needs distinct names (e.g. same commit, different --opt)
        a, b = (a[0], a[1] + "-a"), (b[0], b[1] + "-b")

    book = ensure_book(args.book, args.mode, args.dry_run)
    fastchess = "fastchess" if args.dry_run else _find_fastchess(args.fastchess, bindir)
    cmd = build_fastchess_cmd(fastchess, a, b, book, args)

    if args.dry_run:
        print(" ".join(cmd))
        return 0

    Path(REPO_ROOT / args.pgnout).parent.mkdir(parents=True, exist_ok=True)
    _log(f"{args.mode}: {a[1]} vs {b[1]}")
    rc, text = stream(cmd)
    return report(args.mode, rc, text)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
