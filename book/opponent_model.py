"""Opponent model: predict an opponent's replies from their history, and turn
that into per-opponent weights for our candidate moves.

For a predictable opponent the generic book undersells the moves that punish
their habits, because Stockfish scores each candidate against *best* play, not
against what this opponent actually plays. The model fixes that without any
forced move:

1. **Predict replies.** In a position this opponent has faced before, use their
   reply frequencies there, weighted toward recent games. Otherwise, if in recent games they almost always
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
SIGNATURE_PROB_STEP = 0.05  # reply-probability resolution of the model cache key


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
    ``model_pattern_share``. A habit the opponent just dropped is not a pattern:
    if their latest ``model_pattern_break_games`` games at that ply all played
    something else, the ply is skipped even when the share still qualifies.
    """
    patterns: dict[tuple[str, int], str] = {}
    for color in ("white", "black"):
        rows = conn.execute(
            "SELECT gm.ply, gm.move FROM game_moves gm "
            "JOIN (SELECT id, created_at FROM games WHERE opponent = ? AND color = ? "
            "      ORDER BY created_at DESC LIMIT ?) g ON g.id = gm.game_id "
            "WHERE gm.by_us = 0 AND gm.ply < ? ORDER BY g.created_at DESC",
            (opponent, color, int(settings["model_recent_games"]),
             int(settings["model_pattern_max_ply"]))).fetchall()
        by_ply: dict[int, list[str]] = {}
        for row in rows:
            by_ply.setdefault(row["ply"], []).append(row["move"])
        for ply, newest_first in by_ply.items():
            move = _ply_pattern(newest_first, settings)
            if move is not None:
                patterns[(color, ply)] = move
    return patterns


def _ply_pattern(newest_first: list[str], settings: dict) -> str | None:
    """The opponent's habitual move at one ply (their moves, newest first), or None."""
    top_move = max(set(newest_first), key=newest_first.count)
    share = newest_first.count(top_move) / len(newest_first)
    if (len(newest_first) < int(settings["model_pattern_min_games"])
            or share < float(settings["model_pattern_share"])):
        return None
    latest = newest_first[:int(settings["model_pattern_break_games"])]
    if latest and top_move not in latest:
        return None  # dropped recently
    return top_move


def _game_weight(age: int, half_life: float) -> float:
    """Weight of a game ``age`` games old (0 = newest); no decay if ``half_life`` ≤ 0."""
    if half_life <= 0.0:
        return 1.0
    return 0.5 ** (age / half_life)


def history_weights(conn: sqlite3.Connection, opponent: str, board: chess.Board,
                    half_life: float) -> dict[str, float]:
    """Recency-weighted counts of ``opponent``'s legal moves in ``board``.

    A game's age is its rank among this opponent's games with the same color
    for us, newest first, so habits fade at the pace that opponent plays us
    with that color. A move from ``age`` games ago counts
    ``0.5 ** (age / half_life)``.
    """
    rows = conn.execute(
        "SELECT gm.move, r.age FROM game_moves gm JOIN ("
        "  SELECT id, ROW_NUMBER() OVER (PARTITION BY color ORDER BY created_at DESC) - 1 AS age"
        "  FROM games WHERE opponent = ?) r ON r.id = gm.game_id "
        "WHERE gm.key = ? AND gm.by_us = 0",
        (opponent, db.position_key(board))).fetchall()
    weights: dict[str, float] = {}
    for row in rows:
        if chess.Move.from_uci(row["move"]) not in board.legal_moves:
            continue
        weights[row["move"]] = weights.get(row["move"], 0.0) + _game_weight(row["age"], half_life)
    return weights


def predict_replies(conn: sqlite3.Connection, opponent: str, board: chess.Board,
                    our_color: chess.Color, patterns: dict[tuple[str, int], str],
                    half_life: float) -> list[Reply]:
    """Predicted replies of ``opponent`` (to move in ``board``), most likely first.

    Exact-position history wins, weighted toward recent games
    ([`history_weights`](#history_weights)); otherwise the ply pattern, if its
    move is legal here. An empty list means the model has no prediction.
    """
    weights = history_weights(conn, opponent, board, half_life)
    if weights:
        total = sum(weights.values())
        ranked = sorted(weights.items(), key=lambda item: (-item[1], item[0]))
        return [Reply(move, weight / total, "history") for move, weight in ranked]
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
    """Cache key of a prediction. Probabilities are rounded to
    ``SIGNATURE_PROB_STEP``, so recency decay drifting them slightly after each
    game does not force a Stockfish re-run."""
    def rounded(prob: float) -> float:
        return round(prob / SIGNATURE_PROB_STEP) * SIGNATURE_PROB_STEP

    return " ".join(f"{r.move}:{rounded(r.prob):.2f}" for r in replies)


def _score_candidate(conn: sqlite3.Connection, stockfish: Stockfish, opponent: str,
                     board: chess.Board, move: chess.Move, our_color: chess.Color,
                     patterns: dict[tuple[str, int], str], settings: dict) -> tuple[float, list[Reply]] | None:
    """Expected score of ``move`` against the predicted replies, cached by reply set.

    Returns None when the model cannot predict the reply (nothing to add over
    the generic book), and deletes any value stored from an earlier prediction.
    """
    after = board.copy()
    after.push(move)
    replies = predict_replies(conn, opponent, after, our_color, patterns,
                              float(settings["model_half_life_games"]))
    key = db.position_key(board)
    if not replies:
        conn.execute("DELETE FROM opponent_model WHERE opponent = ? AND key = ? AND move = ?",
                     (opponent, key, move.uci()))  # a stale prediction must not keep weighting
        return None
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


def black_roots(conn: sqlite3.Connection, opponent: str, patterns: dict[tuple[str, int], str],
                settings: dict) -> list[chess.Board]:
    """Positions after each first move ``opponent`` plays as White with a
    predicted probability of at least ``model_root_min_prob``, most likely
    first. An opponent who mixes first moves gets every main one modelled."""
    min_prob = float(settings["model_root_min_prob"])
    first = predict_replies(conn, opponent, chess.Board(), chess.BLACK, patterns,
                            float(settings["model_half_life_games"]))
    roots = []
    for reply in first:
        if reply.prob < min_prob and roots:
            break
        root = chess.Board()
        root.push(chess.Move.from_uci(reply.move))
        roots.append(root)
    return roots


def deepen(conn: sqlite3.Connection, stockfish: Stockfish, opponent: str, settings: dict,
           ensure_candidates: Callable[[chess.Board], None]) -> int:
    """Walk the predicted lines against ``opponent`` and refresh the model values.

    Breadth-first from the start position for each of our colors (as Black, the
    roots are after each likely first move of theirs, see
    [`black_roots`](#black_roots)). At each of our positions below
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
    frontier += [(root, chess.BLACK) for root in black_roots(conn, opponent, patterns, settings)]
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
