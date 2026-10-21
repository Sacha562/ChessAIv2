"""Choose the book move for one in-game request from lichess-bot.

Order of precedence: (1) a prep move set for this opponent in this position;
(2) replay a won game against this opponent whose move list matches the current
game so far, if Stockfish rates the replayed move close enough to the best; (3) inside the book horizon, Thompson-sample among the playable
candidates of this position; (4) otherwise return no move so the engine
searches. Every served move and every miss is logged to
``book_events`` (the source of each game's book-exit ply), and a miss on an
unanalysed position queues it for Stockfish. See [selector.py.md](selector.py.md).
"""
from __future__ import annotations

import random
import sqlite3
from dataclasses import dataclass, field

import chess

from . import db, ingest, opponent_model, prep, scoring, stats


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
    model_expected: float | None = None
    model_reply: str | None = None
    model_source: str | None = None
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
    """Candidates of ``board`` with evidence, posteriors and the tier verdict.

    The opponent-model value of a move (if any) is added to this opponent's
    evidence as ``k_model`` pseudo-games before the posterior is formed, so the
    model moves the weights but real results keep correcting it. A move the
    model rates at least ``model_promote_min`` is playable even above the
    unlocked tier.
    """
    key = db.position_key(board)
    pooled, mine = _evidence(conn, key, opponent, speed, settings)
    model = opponent_model.model_values(conn, opponent, key) if opponent else {}
    k_sf, k_pool = float(settings["k_stockfish"]), float(settings["k_pool"])
    k_model = float(settings["k_model"])
    views: list[CandidateView] = []
    for row in conn.execute("SELECT * FROM candidates WHERE key = ? ORDER BY tier, sf_expected DESC",
                            (key,)):
        if chess.Move.from_uci(row["move"]) not in board.legal_moves:
            continue
        p_stats = pooled.get(row["move"], stats.MoveStats())
        o_stats = mine.get(row["move"], stats.MoveStats())
        modelled = model.get(row["move"])
        evidence = stats.MoveStats(o_stats.n, o_stats.total)
        if modelled is not None:
            evidence.add(modelled["expected"], k_model)
        pooled_post, opponent_post = stats.layered_posterior(row["sf_expected"], p_stats,
                                                             evidence, k_sf, k_pool)
        views.append(CandidateView(
            move=row["move"], tier=row["tier"], source=row["source"], enabled=bool(row["enabled"]),
            sf_expected=row["sf_expected"], sf_cp=row["sf_cp"], sf_mate=row["sf_mate"],
            pooled=p_stats, opponent=o_stats, pooled_post=pooled_post,
            opponent_post=opponent_post,
            model_expected=modelled["expected"] if modelled is not None else None,
            model_reply=modelled["reply"] if modelled is not None else None,
            model_source=modelled["source"] if modelled is not None else None))
    threshold = float(settings["exhaust_threshold"])
    min_games = float(settings["exhaust_min_games"])
    promote_min = float(settings["model_promote_min"])
    enabled = [v for v in views if v.enabled]

    def tier_input(view: CandidateView) -> stats.TierInput:
        model_n = k_model if view.model_expected is not None else 0.0
        return stats.TierInput(view.tier, view.opponent.n + model_n, view.opponent_post.mean)

    tier_limit, all_exhausted = stats.unlocked_tier([tier_input(v) for v in enabled],
                                                    threshold, min_games)
    for view in enabled:
        view.exhausted = stats.is_exhausted(tier_input(view), threshold, min_games)
        promoted = view.model_expected is not None and view.model_expected >= promote_min
        view.playable = view.tier <= tier_limit or (promoted and not view.exhausted)
    position = conn.execute("SELECT widened FROM positions WHERE key = ?", (key,)).fetchone()
    return PositionAnalysis(key=key, candidates=views, tier_limit=tier_limit,
                            all_exhausted=all_exhausted,
                            widened=bool(position and position["widened"]))


def _won_continuations(conn: sqlite3.Connection, request: Request,
                       settings: dict) -> list[tuple[str, str]]:
    """(next move, game id) of every won game whose moves match so far, newest first.

    Matches games against this opponent with our color and a result of at
    least ``replay_min_result``. With ``replay_same_speed_only`` the game must
    also share this speed.
    """
    sql = ("SELECT id, moves FROM games WHERE opponent = ? AND color = ? AND result >= ? "
           "AND n_plies > ?")
    params: list[object] = [request.opponent, "white" if request.color == chess.WHITE else "black",
                            float(settings["replay_min_result"]), len(request.moves)]
    if settings["replay_same_speed_only"]:
        sql += " AND speed = ?"
        params.append(request.speed)
    ply = len(request.moves)
    continuations = []
    for row in conn.execute(sql + " ORDER BY created_at DESC", params):
        played = row["moves"].split()
        if tuple(played[:ply]) == request.moves:
            continuations.append((played[ply], row["id"]))
    return continuations


def replay_loss(conn: sqlite3.Connection, key: int, move: str) -> float | None:
    """Expected-score loss of ``move`` against the best analysed move in ``key``.

    None when Stockfish has not evaluated ``move`` in this position yet.
    """
    row = conn.execute("SELECT MAX(sf_expected) AS best, "
                       "MAX(CASE WHEN move = ? THEN sf_expected END) AS ours "
                       "FROM candidates WHERE key = ?", (move, key)).fetchone()
    if row["best"] is None or row["ours"] is None:
        return None
    return float(row["best"]) - float(row["ours"])


def _queue_replay_check(conn: sqlite3.Connection, board: chess.Board, ply: int) -> None:
    """Queue Stockfish on ``board`` so its replay moves get an eval next time.

    An analysed position is re-queued: the run evaluates every move we have
    played there, which includes the won game's move.
    """
    key = db.position_key(board)
    row = conn.execute("SELECT analysed_at FROM positions WHERE key = ?", (key,)).fetchone()
    if row is None or row["analysed_at"] is None:
        ingest.queue_candidates(conn, board, ply, priority=ingest.PRIORITY_MISS)
    else:
        db.enqueue(conn, "candidates", str(key), ingest.PRIORITY_MISS, fen=board.fen(), force=True)
    conn.commit()


def replay_move(conn: sqlite3.Connection, request: Request, board: chess.Board,
                settings: dict) -> tuple[str, str, float] | None:
    """(move, won game id, loss) continuing a won game whose moves match so far, else None.

    Each distinct legal continuation, newest game first, must pass the eval
    gate: Stockfish rates it at most ``replay_max_loss`` expected score below
    the best move here. A continuation without an eval is skipped this time and
    its position is queued for analysis.
    """
    key = db.position_key(board)
    max_loss = float(settings["replay_max_loss"])
    tried: set[str] = set()
    for move, game_id in _won_continuations(conn, request, settings):
        if move in tried or chess.Move.from_uci(move) not in board.legal_moves:
            continue
        tried.add(move)
        loss = replay_loss(conn, key, move)
        if loss is None:
            _queue_replay_check(conn, board, len(request.moves))
        elif loss <= max_loss:
            return move, game_id, loss
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
    prepared = prep.prep_move(conn, request.opponent, key)
    if prepared is not None and chess.Move.from_uci(prepared) in board.legal_moves:
        _log(conn, request, key, prepared, "prep")
        return Decision(prepared, "prep")
    if settings["replay_enabled"]:
        replay = replay_move(conn, request, board, settings)
        if replay is not None:
            move, game_id, loss = replay
            _log(conn, request, key, move, "replay")
            return Decision(move, "replay", {"game": game_id, "loss": loss})
    if len(request.moves) >= int(settings["max_book_ply"]):
        return Decision(None, "horizon")
    return _book_pick(conn, request, board, key, settings, rng)
