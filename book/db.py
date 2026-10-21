"""SQLite storage for the opening book.

One database file holds every ingested game, the per-ply position keys, the
Stockfish candidates, the analysis queue, the per-ply eval signal, the log of
moves the book served, and the dashboard settings. Only the Windows-side server
process opens it, so SQLite locking never crosses the Docker boundary. See
[db.py.md](db.py.md).
"""
from __future__ import annotations

import sqlite3
import time
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

import chess
import chess.polyglot

SCHEMA = """
CREATE TABLE IF NOT EXISTS games (
    id              TEXT PRIMARY KEY,
    created_at      INTEGER NOT NULL,
    speed           TEXT NOT NULL,
    rated           INTEGER NOT NULL,
    color           TEXT NOT NULL,
    opponent        TEXT NOT NULL,
    opponent_name   TEXT NOT NULL,
    opponent_rating INTEGER,
    opponent_is_bot INTEGER NOT NULL,
    result          REAL NOT NULL,
    status          TEXT NOT NULL,
    n_plies         INTEGER NOT NULL,
    moves           TEXT NOT NULL,
    exit_ply        INTEGER NOT NULL,
    exit_source     TEXT NOT NULL,
    ingested_at     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS games_opponent ON games(opponent, speed);

CREATE TABLE IF NOT EXISTS game_moves (
    game_id TEXT NOT NULL,
    ply     INTEGER NOT NULL,
    key     INTEGER NOT NULL,
    move    TEXT NOT NULL,
    by_us   INTEGER NOT NULL,
    PRIMARY KEY (game_id, ply)
);
CREATE INDEX IF NOT EXISTS game_moves_key ON game_moves(key, by_us);

CREATE TABLE IF NOT EXISTS positions (
    key              INTEGER PRIMARY KEY,
    fen              TEXT NOT NULL,
    best_expected    REAL,
    analysed_depth   INTEGER,
    analysed_multipv INTEGER,
    analysed_at      INTEGER,
    widened          INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS candidates (
    key         INTEGER NOT NULL,
    move        TEXT NOT NULL,
    tier        INTEGER NOT NULL,
    source      TEXT NOT NULL,
    sf_cp       INTEGER,
    sf_mate     INTEGER,
    sf_expected REAL,
    sf_depth    INTEGER,
    enabled     INTEGER NOT NULL DEFAULT 1,
    added_at    INTEGER NOT NULL,
    PRIMARY KEY (key, move)
);

CREATE TABLE IF NOT EXISTS analysis_queue (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    kind         TEXT NOT NULL,
    target       TEXT NOT NULL,
    fen          TEXT,
    priority     INTEGER NOT NULL,
    status       TEXT NOT NULL,
    error        TEXT,
    requested_at INTEGER NOT NULL,
    finished_at  INTEGER,
    UNIQUE (kind, target)
);
CREATE INDEX IF NOT EXISTS analysis_queue_status ON analysis_queue(status, priority);

CREATE TABLE IF NOT EXISTS signal_evals (
    game_id  TEXT NOT NULL,
    ply      INTEGER NOT NULL,
    expected REAL NOT NULL,
    PRIMARY KEY (game_id, ply)
);

CREATE TABLE IF NOT EXISTS book_events (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    game_id    TEXT NOT NULL,
    ply        INTEGER NOT NULL,
    key        INTEGER NOT NULL,
    move       TEXT,
    source     TEXT NOT NULL,
    opponent   TEXT NOT NULL,
    speed      TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS book_events_game ON book_events(game_id, ply);

CREATE TABLE IF NOT EXISTS settings (
    name  TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS meta (
    name  TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
"""

_UINT64_SIGN = 1 << 63
_UINT64_RANGE = 1 << 64


def position_key(board: chess.Board) -> int:
    """Polyglot Zobrist hash of ``board`` as a signed 64-bit SQLite integer.

    Polyglot hashing ignores move counters and only counts an en-passant square
    when a capture is possible, so transpositions share one key.
    """
    key = chess.polyglot.zobrist_hash(board)
    return key - _UINT64_RANGE if key >= _UINT64_SIGN else key


def now() -> int:
    """Current Unix time in whole seconds."""
    return int(time.time())


def connect(path: Path) -> sqlite3.Connection:
    """Open (creating if needed) the database at ``path`` with the schema applied."""
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(path, timeout=30.0, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.executescript(SCHEMA)
    return conn


@contextmanager
def session(path: Path) -> Iterator[sqlite3.Connection]:
    """Yield a fresh connection and close it afterwards (one per thread/request)."""
    conn = connect(path)
    try:
        yield conn
    finally:
        conn.close()


def get_meta(conn: sqlite3.Connection, name: str, default: str | None = None) -> str | None:
    """Read one value from the ``meta`` key/value table."""
    row = conn.execute("SELECT value FROM meta WHERE name = ?", (name,)).fetchone()
    return row["value"] if row else default


def set_meta(conn: sqlite3.Connection, name: str, value: str) -> None:
    """Write one value to the ``meta`` key/value table (caller commits)."""
    conn.execute("INSERT INTO meta(name, value) VALUES(?, ?) "
                 "ON CONFLICT(name) DO UPDATE SET value = excluded.value", (name, value))


def enqueue(conn: sqlite3.Connection, kind: str, target: str, priority: int,
            fen: str | None = None, force: bool = False) -> None:
    """Queue an analysis job, keyed by (``kind``, ``target``).

    An already-queued job keeps its place and takes the lower (more urgent) of
    the two priorities. A finished or failed job is re-queued only when
    ``force`` is true (a manual "analyse again"); otherwise it is left alone so
    ingest can call this for every position without repeating work. Lower
    ``priority`` runs first. The caller commits.
    """
    requeue = "'queued'" if force else "status"
    error = "NULL" if force else "error"
    conn.execute(
        "INSERT INTO analysis_queue(kind, target, fen, priority, status, requested_at) "
        "VALUES(?, ?, ?, ?, 'queued', ?) "
        "ON CONFLICT(kind, target) DO UPDATE SET "
        "  priority = MIN(priority, excluded.priority), "
        f" status = CASE WHEN status IN ('queued', 'running') THEN status ELSE {requeue} END, "
        f" error = {error}, "
        "  fen = COALESCE(excluded.fen, fen), "
        "  requested_at = excluded.requested_at",
        (kind, target, fen, priority, now()))
