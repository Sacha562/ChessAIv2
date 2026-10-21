"""Opponent model: predict an opponent's replies from their history, and turn
that into per-opponent weights for our candidate moves.

For a predictable opponent the generic book undersells the moves that punish
their habits, because Stockfish scores each candidate against *best* play, not
against what this opponent actually plays. The model fixes that without any
forced move:

1. **Predict replies.** In a position this opponent has faced before, use their
   reply frequencies there. Otherwise, if in recent games they almost always
   played the same move at this ply with this color (e.g. 2...Ke7), predict it.
2. **Deepen.** From the start position, walk our candidate moves and the
   predicted replies, analysing new positions with Stockfish, and score each
   candidate by Stockfish's expected score *after the predicted reply*.
3. **Weight.** That score enters the opponent layer of the posterior as
   ``k_model`` pseudo-games ([selector.py.md](selector.py.md)). Real results
   against the opponent keep updating it like any other evidence.

See [opponent_model.py.md](opponent_model.py.md).
"""
from __future__ import annotations

import sqlite3
from collections.abc import Callable
from dataclasses import dataclass

import chess

from . import db
from .engine import Stockfish

DETERMINISM_MAX_PLY = 40


@dataclass(frozen=True)
class Reply:
    """One predicted opponent reply: UCI move, probability, and where it came from."""

    move: str
    prob: float
    source: str  # 'history' (this exact position) or 'pattern' (same move at this ply)


def determinism(conn: sqlite3.Connection, opponent: str) -> tuple[float | None, int]:
    """Share of repeated positions (before ply 40) where ``opponent`` always
    chose the same move, and how many repeated positions there were."""
    rows = conn.execute(
        "SELECT COUNT(DISTINCT gm.move) AS distinct_moves FROM game_moves gm "
        "JOIN games g ON g.id = gm.game_id WHERE g.opponent = ? AND gm.by_us = 0 "
        "AND gm.ply < ? GROUP BY gm.key HAVING COUNT(*) >= 2",
        (opponent, DETERMINISM_MAX_PLY)).fetchall()
    if not rows:
        return None, 0
    same = sum(1 for r in rows if r["distinct_moves"] == 1)
    return same / len(rows), len(rows)


def ply_patterns(conn: sqlite3.Connection, opponent: str, settings: dict) -> dict[tuple[str, int], str]:
    """``{(our color, ply): move}`` for plies where the opponent's recent games agree.

    Uses the opponent's ``model_recent_games`` most recent games per our color,
    plies below ``model_pattern_max_ply``; a ply qualifies with at least
    ``model_pattern_min_games`` games and a top-move share of at least
    ``model_pattern_share``.
    """
    patterns: dict[tuple[str, int], str] = {}
    for color in ("white", "black"):
        rows = conn.execute(
            "SELECT gm.ply, gm.move, COUNT(*) AS n FROM game_moves gm "
            "JOIN (SELECT id FROM games WHERE opponent = ? AND color = ? "
            "      ORDER BY created_at DESC LIMIT ?) g ON g.id = gm.game_id "
            "WHERE gm.by_us = 0 AND gm.ply < ? GROUP BY gm.ply, gm.move",
            (opponent, color, int(settings["model_recent_games"]),
             int(settings["model_pattern_max_ply"]))).fetchall()
        by_ply: dict[int, list[tuple[int, str]]] = {}
        for row in rows:
            by_ply.setdefault(row["ply"], []).append((row["n"], row["move"]))
        for ply, counts in by_ply.items():
            total = sum(n for n, _ in counts)
            top_n, top_move = max(counts)
            if (total >= int(settings["model_pattern_min_games"])
                    and top_n / total >= float(settings["model_pattern_share"])):
                patterns[(color, ply)] = top_move
    return patterns


def predict_replies(conn: sqlite3.Connection, opponent: str, board: chess.Board,
                    our_color: chess.Color, patterns: dict[tuple[str, int], str]) -> list[Reply]:
    """Predicted replies of ``opponent`` (to move in ``board``), most likely first.

    Exact-position history wins; otherwise the ply pattern, if its move is legal
    here. An empty list means the model has no prediction.
    """
    rows = conn.execute(
        "SELECT gm.move, COUNT(*) AS n FROM game_moves gm JOIN games g ON g.id = gm.game_id "
        "WHERE g.opponent = ? AND gm.key = ? AND gm.by_us = 0 GROUP BY gm.move ORDER BY n DESC",
        (opponent, db.position_key(board))).fetchall()
    legal = [(r["move"], r["n"]) for r in rows if chess.Move.from_uci(r["move"]) in board.legal_moves]
    if legal:
        total = sum(n for _, n in legal)
        return [Reply(move, n / total, "history") for move, n in legal]
    color = "white" if our_color == chess.WHITE else "black"
    move = patterns.get((color, board.ply()))
    if move is not None and chess.Move.from_uci(move) in board.legal_moves:
        return [Reply(move, 1.0, "pattern")]
    return []


def model_values(conn: sqlite3.Connection, opponent: str, key: int) -> dict[str, sqlite3.Row]:
    """Stored model rows for our moves in position ``key`` against ``opponent``."""
    return {r["move"]: r for r in conn.execute(
        "SELECT * FROM opponent_model WHERE opponent = ? AND key = ?", (opponent, key))}


def _reply_signature(replies: list[Reply]) -> str:
    return " ".join(f"{r.move}:{r.prob:.3f}" for r in replies)


def _score_candidate(conn: sqlite3.Connection, stockfish: Stockfish, opponent: str,
                     board: chess.Board, move: chess.Move, our_color: chess.Color,
                     patterns: dict[tuple[str, int], str], settings: dict) -> tuple[float, list[Reply]] | None:
    """Expected score of ``move`` against the predicted replies, cached by reply set.

    Returns None when the model cannot predict the reply (nothing to add over
    the generic book).
    """
    after = board.copy()
    after.push(move)
    replies = predict_replies(conn, opponent, after, our_color, patterns)
    if not replies:
        return None
    key = db.position_key(board)
    signature = _reply_signature(replies)
    cached = conn.execute("SELECT expected, replies FROM opponent_model WHERE opponent = ? "
                          "AND key = ? AND move = ?", (opponent, key, move.uci())).fetchone()
    if cached is not None and cached["replies"] == signature:
        return cached["expected"], replies
    nodes = int(settings["model_nodes"])
    expected = 0.0
    for reply in replies:
        child = after.copy()
        child.push(chess.Move.from_uci(reply.move))
        expected += reply.prob * _position_value(stockfish, child, nodes, our_color)
    conn.execute(
        "INSERT INTO opponent_model(opponent, key, move, expected, reply, reply_prob, source, "
        "replies, updated_at) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(opponent, key, move) DO UPDATE "
        "SET expected = excluded.expected, reply = excluded.reply, reply_prob = excluded.reply_prob, "
        "source = excluded.source, replies = excluded.replies, updated_at = excluded.updated_at",
        (opponent, key, move.uci(), expected, replies[0].move, replies[0].prob, replies[0].source,
         signature, db.now()))
    return expected, replies


def _position_value(stockfish: Stockfish, board: chess.Board, nodes: int,
                    our_color: chess.Color) -> float:
    """Expected score for us; decided positions use the game result."""
    outcome = board.outcome()
    if outcome is not None:
        return 0.5 if outcome.winner is None else float(outcome.winner == our_color)
    return stockfish.position_expected(board, nodes, our_color)


def deepen(conn: sqlite3.Connection, stockfish: Stockfish, opponent: str, settings: dict,
           ensure_candidates: Callable[[chess.Board], None]) -> int:
    """Walk the predicted lines against ``opponent`` and refresh the model values.

    Breadth-first from the start position for each of our colors (as Black, the
    root is after their predicted first move). At each of our positions below
    ``max_book_ply`` it makes sure the position has candidates
    (``ensure_candidates`` runs Stockfish if needed), scores every enabled
    candidate against the predicted replies, and continues along the
    ``model_branch`` best-scoring candidates with their most likely reply. At
    most ``model_max_positions`` positions are expanded per call; commits per
    position. Returns the number of positions expanded.
    """
    patterns = ply_patterns(conn, opponent, settings)
    budget = int(settings["model_max_positions"])
    max_ply = int(settings["max_book_ply"])
    branch = int(settings["model_branch"])
    frontier: list[tuple[chess.Board, chess.Color]] = [(chess.Board(), chess.WHITE)]
    first = predict_replies(conn, opponent, chess.Board(), chess.BLACK, patterns)
    if first:
        black_root = chess.Board()
        black_root.push(chess.Move.from_uci(first[0].move))
        frontier.append((black_root, chess.BLACK))
    expanded = 0
    seen: set[tuple[int, bool]] = set()
    while frontier and expanded < budget:
        board, our_color = frontier.pop(0)
        node = (db.position_key(board), our_color)
        if board.ply() >= max_ply or node in seen or board.is_game_over():
            continue
        seen.add(node)
        ensure_candidates(board)
        expanded += 1
        moves = [chess.Move.from_uci(r["move"]) for r in conn.execute(
            "SELECT move FROM candidates WHERE key = ? AND enabled = 1", (node[0],))]
        scored = []
        for move in (m for m in moves if m in board.legal_moves):
            result = _score_candidate(conn, stockfish, opponent, board, move, our_color,
                                      patterns, settings)
            if result is not None:
                scored.append((result[0], move, result[1]))
        conn.commit()
        for _, move, replies in sorted(scored, key=lambda s: s[0], reverse=True)[:branch]:
            child = board.copy()
            child.push(move)
            child.push(chess.Move.from_uci(replies[0].move))
            frontier.append((child, our_color))
    return expanded
