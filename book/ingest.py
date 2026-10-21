"""Turn exported lichess games into book data.

Each finished standard game of the bot account becomes one ``games`` row plus
one ``game_moves`` row per ply (position key before the move, the move, and
whether we played it). Ingest also queues the follow-up Stockfish work: an eval
signal for the game, and candidate generation for every position where we were
to move inside the book horizon that has not been analysed yet. See
[ingest.py.md](ingest.py.md).
"""
from __future__ import annotations

import logging
import sqlite3
from dataclasses import dataclass

import chess

from . import db, lichess_api

logger = logging.getLogger(__name__)

SKIPPED_STATUSES = {"created", "started", "aborted", "noStart", "unknownFinish"}
SPEED_ORDER = ("ultraBullet", "bullet", "blitz", "rapid", "classical", "correspondence")

PRIORITY_MISS = 0
PRIORITY_MANUAL = 1
PRIORITY_WIDEN = 5
PRIORITY_CANDIDATE_BASE = 10
PRIORITY_SIGNAL = 60


@dataclass(frozen=True)
class ParsedGame:
    """One finished game, seen from the bot's side."""

    id: str
    created_at: int
    speed: str
    rated: bool
    color: chess.Color
    opponent: str
    opponent_name: str
    opponent_rating: int | None
    opponent_is_bot: bool
    result: float
    status: str
    moves: tuple[chess.Move, ...]


def _player_id(player: dict) -> str:
    return ((player.get("user") or {}).get("id") or "").lower()


def parse_game(raw: dict, account: str) -> ParsedGame | None:
    """Convert one lichess export object into a ParsedGame.

    Returns None for games the book cannot learn from: variants, games from a
    custom start position, unfinished or aborted games, games the account did
    not play, and games whose move list does not replay legally.
    """
    if raw.get("variant") != "standard" or raw.get("initialFen"):
        return None
    if raw.get("status") in SKIPPED_STATUSES:
        return None
    white, black = raw["players"]["white"], raw["players"]["black"]
    me = account.lower()
    if _player_id(white) == me:
        color, opponent = chess.WHITE, black
    elif _player_id(black) == me:
        color, opponent = chess.BLACK, white
    else:
        return None
    board = chess.Board()
    moves: list[chess.Move] = []
    try:
        for san in (raw.get("moves") or "").split():
            move = board.parse_san(san)
            board.push(move)
            moves.append(move)
    except ValueError:
        logger.warning("game %s: unparseable move list, skipped", raw.get("id"))
        return None
    winner = raw.get("winner")
    result = 0.5 if winner is None else (1.0 if (winner == "white") == color else 0.0)
    user = opponent.get("user") or {}
    return ParsedGame(
        id=raw["id"], created_at=int(raw["createdAt"]), speed=raw.get("speed", "unknown"),
        rated=bool(raw.get("rated")), color=color, opponent=_player_id(opponent),
        opponent_name=user.get("name") or "anonymous", opponent_rating=opponent.get("rating"),
        opponent_is_bot=user.get("title") == "BOT", result=result,
        status=raw.get("status", ""), moves=tuple(moves))


def our_plies(color: chess.Color, n_plies: int) -> range:
    """Plies (0-based half-move indices) on which ``color`` moves."""
    return range(0 if color == chess.WHITE else 1, n_plies, 2)


def exit_ply(conn: sqlite3.Connection, game: ParsedGame, max_book_ply: int) -> tuple[int, str]:
    """Return (ply, source) where the game left the book.

    With a served-move log for the game, the exit is our first move the book
    or replay did not supply (``'served'``). Without one (older games, or the
    server was down) it is the book horizon, capped at the game length
    (``'inferred'``).
    """
    served = {row["ply"] for row in conn.execute(
        "SELECT ply FROM book_events WHERE game_id = ? AND source IN ('book', 'replay')",
        (game.id,))}
    n_plies = len(game.moves)
    if not served:
        return min(max_book_ply, n_plies), "inferred"
    for ply in our_plies(game.color, n_plies):
        if ply not in served:
            return ply, "served"
    return n_plies, "served"


def store_game(conn: sqlite3.Connection, game: ParsedGame, settings: dict) -> None:
    """Insert one game (replacing any earlier copy) and queue its analysis."""
    max_book_ply = int(settings["max_book_ply"])
    exit_at, exit_source = exit_ply(conn, game, max_book_ply)
    conn.execute("DELETE FROM game_moves WHERE game_id = ?", (game.id,))
    conn.execute(
        "INSERT OR REPLACE INTO games(id, created_at, speed, rated, color, opponent, "
        "opponent_name, opponent_rating, opponent_is_bot, result, status, n_plies, moves, "
        "exit_ply, exit_source, ingested_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (game.id, game.created_at, game.speed, int(game.rated),
         "white" if game.color == chess.WHITE else "black", game.opponent,
         game.opponent_name, game.opponent_rating, int(game.opponent_is_bot), game.result,
         game.status, len(game.moves), " ".join(m.uci() for m in game.moves),
         exit_at, exit_source, db.now()))
    board = chess.Board()
    for ply, move in enumerate(game.moves):
        key = db.position_key(board)
        by_us = board.turn == game.color
        conn.execute("INSERT INTO game_moves(game_id, ply, key, move, by_us) VALUES(?,?,?,?,?)",
                     (game.id, ply, key, move.uci(), int(by_us)))
        if by_us and ply < max_book_ply and worth_analysing(conn, key, ply, settings):
            queue_candidates(conn, board, ply)
        board.push(move)
    db.enqueue(conn, "signal", game.id, PRIORITY_SIGNAL)


def worth_analysing(conn: sqlite3.Connection, key: int, ply: int, settings: dict) -> bool:
    """Whether a position from game history earns candidate analysis.

    Shallow positions (ply < ``candidate_always_ply``) always do. Deeper ones
    must have been reached on our move in at least ``candidate_min_visits``
    games, so one-off positions deep in a line do not flood the queue; a live
    book miss queues its position regardless (see selector).
    """
    if ply < int(settings["candidate_always_ply"]):
        return True
    visits = conn.execute("SELECT COUNT(*) FROM game_moves WHERE key = ? AND by_us = 1",
                          (key,)).fetchone()[0]
    return visits >= int(settings["candidate_min_visits"])


def rebuild_candidate_queue(conn: sqlite3.Connection, settings: dict) -> int:
    """Drop queued (not running) candidate jobs and re-queue them from history
    under the current visit policy. Returns the number of jobs queued."""
    conn.execute("DELETE FROM analysis_queue WHERE kind = 'candidates' AND status = 'queued'")
    max_book_ply = int(settings["max_book_ply"])
    rows = conn.execute(
        "SELECT g.moves, MIN(gm.ply) AS ply, gm.key FROM game_moves gm "
        "JOIN games g ON g.id = gm.game_id WHERE gm.by_us = 1 AND gm.ply < ? "
        "GROUP BY gm.key", (max_book_ply,)).fetchall()
    queued = 0
    for row in rows:
        if not worth_analysing(conn, row["key"], row["ply"], settings):
            continue
        board = chess.Board()
        for uci in row["moves"].split()[:row["ply"]]:
            board.push_uci(uci)
        if db.position_key(board) != row["key"]:  # reached by transposition at another ply
            continue
        queue_candidates(conn, board, row["ply"])
        queued += 1
    conn.commit()
    return queued


def queue_candidates(conn: sqlite3.Connection, board: chess.Board, ply: int,
                     priority: int | None = None) -> None:
    """Queue candidate generation for ``board`` unless it is already analysed."""
    key = db.position_key(board)
    row = conn.execute("SELECT analysed_at FROM positions WHERE key = ?", (key,)).fetchone()
    if row is not None and row["analysed_at"] is not None:
        return
    conn.execute("INSERT OR IGNORE INTO positions(key, fen) VALUES(?, ?)", (key, board.fen()))
    db.enqueue(conn, "candidates", str(key), PRIORITY_CANDIDATE_BASE + ply if priority is None
               else priority, fen=board.fen())


def ingest_new(conn: sqlite3.Connection, settings: dict) -> int:
    """Download and store every game finished since the last ingest.

    Progress is committed per game, so an interrupted ingest resumes where it
    stopped. Returns the number of games stored. Raises
    lichess_api.RateLimited when lichess asks us to back off.
    """
    account = str(settings["account"])
    since = int(db.get_meta(conn, "last_created_at", "0") or 0)
    stored = 0
    for raw in lichess_api.export_games(account, since + 1):
        game = parse_game(raw, account)
        if game is not None:
            store_game(conn, game, settings)
            stored += 1
        db.set_meta(conn, "last_created_at", str(int(raw["createdAt"])))
        conn.commit()
    db.set_meta(conn, "last_ingest_at", str(db.now()))
    conn.commit()
    return stored
