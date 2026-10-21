"""Choose the book move for one in-game request from lichess-bot.

Order of precedence: (1) replay a won game against this opponent whose move
list matches the current game so far; (2) inside the book horizon, Thompson-
sample among the playable candidates of this position; (3) otherwise return no
move so the engine searches. Every served move and every miss is logged to
``book_events`` (the source of each game's book-exit ply), and a miss on an
unanalysed position queues it for Stockfish. See [selector.py.md](selector.py.md).
"""
from __future__ import annotations

import random
import sqlite3
from dataclasses import dataclass, field

import chess

from . import db, ingest, scoring, stats


@dataclass(frozen=True)
class Request:
    """One move request from the lichess-bot shim."""

    game_id: str
    moves: tuple[str, ...]
    color: chess.Color
    opponent: str
    speed: str


@dataclass(frozen=True)
class Decision:
    """The book's answer: a UCI move, or None to let the engine search."""

    move: str | None
    source: str
    detail: dict = field(default_factory=dict)


@dataclass
class CandidateView:
    """One candidate with its evidence and posteriors (shared with the dashboard)."""

    move: str
    tier: int
    source: str
    enabled: bool
    sf_expected: float | None
    sf_cp: int | None
    sf_mate: int | None
    pooled: stats.MoveStats
    opponent: stats.MoveStats
    pooled_post: stats.Posterior
    opponent_post: stats.Posterior
    exhausted: bool = False
    playable: bool = False


@dataclass
class PositionAnalysis:
    """Everything the book knows about one position for one opponent and speed."""

    key: int
    candidates: list[CandidateView]
    tier_limit: int
    all_exhausted: bool
    widened: bool


def board_from_moves(moves: tuple[str, ...]) -> chess.Board:
    """Replay UCI ``moves`` from the start position. Raises ValueError if illegal."""
    board = chess.Board()
    for uci in moves:
        move = chess.Move.from_uci(uci)
        if move not in board.legal_moves:
            raise ValueError(f"illegal move {uci}")
        board.push(move)
    return board


def _evidence(conn: sqlite3.Connection, key: int, opponent: str, speed: str,
              settings: dict) -> tuple[dict[str, stats.MoveStats], dict[str, stats.MoveStats]]:
    """Per-move (pooled, opponent) evidence for our moves played in ``key``.

    Pooled = other opponents at this speed. Opponent = this opponent at this
    speed (weight 1) and at other speeds (weight ``other_speed_weight``).
    """
    rows = conn.execute(
        "SELECT gm.move, g.id, g.opponent, g.speed, g.result, g.exit_ply, g.n_plies "
        "FROM game_moves gm JOIN games g ON g.id = gm.game_id "
        "WHERE gm.key = ? AND gm.by_us = 1", (key,)).fetchall()
    games = [scoring.GameRow(r["id"], r["result"], r["exit_ply"], r["n_plies"]) for r in rows]
    signals = scoring.game_signals(conn, games, settings)
    other_weight = float(settings["other_speed_weight"])
    pooled: dict[str, stats.MoveStats] = {}
    mine: dict[str, stats.MoveStats] = {}
    for row in rows:
        signal = signals[row["id"]]
        if row["opponent"] == opponent:
            weight = 1.0 if row["speed"] == speed else other_weight
            mine.setdefault(row["move"], stats.MoveStats()).add(signal, weight)
        elif row["speed"] == speed:
            pooled.setdefault(row["move"], stats.MoveStats()).add(signal)
    return pooled, mine


def analyse_position(conn: sqlite3.Connection, board: chess.Board, opponent: str, speed: str,
                     settings: dict) -> PositionAnalysis:
    """Candidates of ``board`` with evidence, posteriors and the tier verdict."""
    key = db.position_key(board)
    pooled, mine = _evidence(conn, key, opponent, speed, settings)
    k_sf, k_pool = float(settings["k_stockfish"]), float(settings["k_pool"])
    views: list[CandidateView] = []
    for row in conn.execute("SELECT * FROM candidates WHERE key = ? ORDER BY tier, sf_expected DESC",
                            (key,)):
        if chess.Move.from_uci(row["move"]) not in board.legal_moves:
            continue
        p_stats = pooled.get(row["move"], stats.MoveStats())
        o_stats = mine.get(row["move"], stats.MoveStats())
        pooled_post, opponent_post = stats.layered_posterior(row["sf_expected"], p_stats,
                                                             o_stats, k_sf, k_pool)
        views.append(CandidateView(
            move=row["move"], tier=row["tier"], source=row["source"], enabled=bool(row["enabled"]),
            sf_expected=row["sf_expected"], sf_cp=row["sf_cp"], sf_mate=row["sf_mate"],
            pooled=p_stats, opponent=o_stats, pooled_post=pooled_post,
            opponent_post=opponent_post))
    threshold = float(settings["exhaust_threshold"])
    min_games = float(settings["exhaust_min_games"])
    enabled = [v for v in views if v.enabled]
    tier_inputs = [stats.TierInput(v.tier, v.opponent.n, v.opponent_post.mean) for v in enabled]
    tier_limit, all_exhausted = stats.unlocked_tier(tier_inputs, threshold, min_games)
    for view in enabled:
        view.exhausted = stats.is_exhausted(
            stats.TierInput(view.tier, view.opponent.n, view.opponent_post.mean), threshold, min_games)
        view.playable = view.tier <= tier_limit
    position = conn.execute("SELECT widened FROM positions WHERE key = ?", (key,)).fetchone()
    return PositionAnalysis(key=key, candidates=views, tier_limit=tier_limit,
                            all_exhausted=all_exhausted,
                            widened=bool(position and position["widened"]))


def replay_move(conn: sqlite3.Connection, request: Request, settings: dict) -> tuple[str, str] | None:
    """(move, won game id) continuing a won game whose moves match so far, else None.

    Matches games against this opponent with our color and a result of at
    least ``replay_min_result``; the most recent such game wins ties. With
    ``replay_same_speed_only`` the game must also share this speed.
    """
    sql = ("SELECT id, moves FROM games WHERE opponent = ? AND color = ? AND result >= ? "
           "AND n_plies > ?")
    params: list[object] = [request.opponent, "white" if request.color == chess.WHITE else "black",
                            float(settings["replay_min_result"]), len(request.moves)]
    if settings["replay_same_speed_only"]:
        sql += " AND speed = ?"
        params.append(request.speed)
    ply = len(request.moves)
    for row in conn.execute(sql + " ORDER BY created_at DESC", params):
        played = row["moves"].split()
        if tuple(played[:ply]) == request.moves:
            return played[ply], row["id"]
    return None


def _log(conn: sqlite3.Connection, request: Request, key: int, move: str | None, source: str) -> None:
    conn.execute("INSERT INTO book_events(game_id, ply, key, move, source, opponent, speed, "
                 "created_at) VALUES(?,?,?,?,?,?,?,?)",
                 (request.game_id, len(request.moves), key, move, source, request.opponent,
                  request.speed, db.now()))
    conn.commit()


def _book_pick(conn: sqlite3.Connection, request: Request, board: chess.Board, key: int,
               settings: dict, rng: random.Random) -> Decision:
    analysis = analyse_position(conn, board, request.opponent, request.speed, settings)
    playable = {v.move: v.opponent_post for v in analysis.candidates if v.enabled and v.playable}
    if analysis.all_exhausted and not analysis.widened:
        db.enqueue(conn, "widen", str(key), ingest.PRIORITY_WIDEN, fen=board.fen())
        conn.commit()
    if not playable:
        ingest.queue_candidates(conn, board, len(request.moves), priority=ingest.PRIORITY_MISS)
        _log(conn, request, key, None, "miss")
        return Decision(None, "miss")
    move = str(stats.thompson_pick(playable, rng))
    _log(conn, request, key, move, "book")
    return Decision(move, "book", {"tier_limit": analysis.tier_limit,
                                   "posterior_mean": playable[move].mean})


def choose(conn: sqlite3.Connection, request: Request, settings: dict,
           rng: random.Random) -> Decision:
    """Answer one shim request. Never raises for a bad request: it returns no move."""
    try:
        board = board_from_moves(request.moves)
    except ValueError as error:
        return Decision(None, "invalid", {"error": str(error)})
    if board.turn != request.color:
        return Decision(None, "not_our_turn")
    key = db.position_key(board)
    if settings["replay_enabled"]:
        replay = replay_move(conn, request, settings)
        if replay is not None and chess.Move.from_uci(replay[0]) in board.legal_moves:
            _log(conn, request, key, replay[0], "replay")
            return Decision(replay[0], "replay", {"game": replay[1]})
    if len(request.moves) >= int(settings["max_book_ply"]):
        return Decision(None, "horizon")
    return _book_pick(conn, request, board, key, settings, rng)
