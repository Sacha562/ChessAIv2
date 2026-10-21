"""Per-opponent prepared moves ("prep") that override replay and the book.

A prep entry says: against this opponent, in this position, always play this
move. It applies at every speed and at any ply, and only in that exact
position (by Polyglot key), so the moment the opponent deviates the normal
replay/book logic takes over again. See [prep.py.md](prep.py.md).
"""
from __future__ import annotations

import sqlite3

import chess

from . import db


def prep_move(conn: sqlite3.Connection, opponent: str, key: int) -> str | None:
    """The prepared UCI move for ``opponent`` in position ``key``, if any."""
    row = conn.execute("SELECT move FROM prep_moves WHERE opponent = ? AND key = ?",
                       (opponent, key)).fetchone()
    return row["move"] if row else None


def _board(moves: tuple[str, ...]) -> chess.Board:
    board = chess.Board()
    for uci in moves:
        move = chess.Move.from_uci(uci)
        if move not in board.legal_moves:
            raise ValueError(f"illegal move {uci}")
        board.push(move)
    return board


def set_prep(conn: sqlite3.Connection, opponent: str, moves: tuple[str, ...], move: str,
             note: str | None = None) -> None:
    """Prepare ``move`` (UCI) after ``moves`` against ``opponent``, replacing any
    earlier prep for that position. Raises ValueError on an illegal move. The
    caller commits."""
    board = _board(moves)
    if chess.Move.from_uci(move) not in board.legal_moves:
        raise ValueError(f"illegal move {move}")
    conn.execute(
        "INSERT INTO prep_moves(opponent, key, moves, move, note, created_at) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(opponent, key) DO UPDATE SET moves = excluded.moves, move = excluded.move, "
        "note = excluded.note, created_at = excluded.created_at",
        (opponent, db.position_key(board), " ".join(moves), move, note, db.now()))


def set_prep_line(conn: sqlite3.Connection, opponent: str, line: tuple[str, ...],
                  color: chess.Color, note: str | None = None) -> int:
    """Prepare every ``color`` move of ``line`` (UCI from the start position)
    against ``opponent``. Returns the number of moves prepared. The caller
    commits."""
    _board(line)  # validate the whole line before writing anything
    first = 0 if color == chess.WHITE else 1
    for ply in range(first, len(line), 2):
        set_prep(conn, opponent, line[:ply], line[ply], note)
    return len(range(first, len(line), 2))


def remove_prep(conn: sqlite3.Connection, opponent: str, moves: tuple[str, ...]) -> bool:
    """Delete the prep for the position after ``moves``; True if one existed.
    The caller commits."""
    key = db.position_key(_board(moves))
    return conn.execute("DELETE FROM prep_moves WHERE opponent = ? AND key = ?",
                        (opponent, key)).rowcount > 0


def _san_path(moves: str, move: str) -> str:
    board = chess.Board()
    sans = []
    for uci in [*moves.split(), move]:
        parsed = chess.Move.from_uci(uci)
        number = f"{board.fullmove_number}." if board.turn == chess.WHITE else ""
        if not sans and board.turn == chess.BLACK:
            number = f"{board.fullmove_number}..."
        sans.append(number + board.san(parsed))
        board.push(parsed)
    return " ".join(sans)


def list_prep(conn: sqlite3.Connection, opponent: str | None = None) -> list[dict]:
    """Prep entries (optionally for one opponent), shallowest first, with the
    position's move list and a SAN rendering of path + prepared move."""
    sql = "SELECT opponent, moves, move, note, created_at FROM prep_moves"
    params: tuple = ()
    if opponent:
        sql += " WHERE opponent = ?"
        params = (opponent,)
    rows = conn.execute(sql + " ORDER BY opponent, LENGTH(moves), moves", params).fetchall()
    return [{"opponent": r["opponent"], "moves": r["moves"].split(), "move": r["move"],
             "note": r["note"], "created_at": r["created_at"],
             "san": _san_path(r["moves"], r["move"])} for r in rows]
