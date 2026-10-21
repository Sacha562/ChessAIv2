"""Read models for the dashboard: overview, opponents, positions, queue.

Every function takes an open connection and returns plain JSON-ready dicts and
lists. Position keys travel as strings because JavaScript numbers cannot hold a
signed 64-bit integer exactly. See [reports.py.md](reports.py.md).
"""
from __future__ import annotations

import random
import sqlite3

import chess

from . import db, scoring, selector, stats

RECENT_EVENTS = 30
RECENT_GAMES = 50
DETERMINISM_MAX_PLY = 40
LINE_PLIES = 6
QUEUE_LIMIT = 200


def _score(results: list[float]) -> float | None:
    return sum(results) / len(results) if results else None


def overview(conn: sqlite3.Connection) -> dict:
    """Totals per speed, analysis coverage, queue counts and recent book moves."""
    speeds = [dict(r) for r in conn.execute(
        "SELECT speed, COUNT(*) AS games, AVG(result) AS score, "
        "COUNT(DISTINCT opponent) AS opponents FROM games GROUP BY speed ORDER BY games DESC")]
    totals = conn.execute("SELECT COUNT(*) AS games, COUNT(DISTINCT opponent) AS opponents, "
                          "AVG(result) AS score FROM games").fetchone()
    queue: dict[str, dict[str, int]] = {}
    for row in conn.execute("SELECT kind, status, COUNT(*) AS n FROM analysis_queue "
                            "GROUP BY kind, status"):
        queue.setdefault(row["kind"], {})[row["status"]] = row["n"]
    coverage = conn.execute(
        "SELECT (SELECT COUNT(*) FROM positions WHERE analysed_at IS NOT NULL) AS analysed, "
        "(SELECT COUNT(*) FROM candidates WHERE enabled = 1) AS candidates, "
        "(SELECT COUNT(*) FROM candidates WHERE source = 'manual') AS manual").fetchone()
    events = [dict(r) for r in conn.execute(
        "SELECT game_id, ply, move, source, opponent, speed, created_at FROM book_events "
        "ORDER BY id DESC LIMIT ?", (RECENT_EVENTS,))]
    return {"totals": dict(totals), "speeds": speeds, "queue": queue, "coverage": dict(coverage),
            "events": events, "last_ingest_at": db.get_meta(conn, "last_ingest_at")}


def _determinism(conn: sqlite3.Connection, opponent: str) -> tuple[float | None, int]:
    """Share of repeated positions where ``opponent`` always chose the same move."""
    rows = conn.execute(
        "SELECT COUNT(DISTINCT gm.move) AS distinct_moves FROM game_moves gm "
        "JOIN games g ON g.id = gm.game_id WHERE g.opponent = ? AND gm.by_us = 0 "
        "AND gm.ply < ? GROUP BY gm.key HAVING COUNT(*) >= 2",
        (opponent, DETERMINISM_MAX_PLY)).fetchall()
    if not rows:
        return None, 0
    same = sum(1 for r in rows if r["distinct_moves"] == 1)
    return same / len(rows), len(rows)


def opponents(conn: sqlite3.Connection) -> list[dict]:
    """One row per opponent: games, score per speed, determinism, replayable wins."""
    result = []
    for row in conn.execute(
            "SELECT opponent, MAX(opponent_name) AS name, MAX(opponent_is_bot) AS is_bot, "
            "COUNT(*) AS games, AVG(result) AS score, MAX(created_at) AS last_played, "
            "SUM(result = 1.0) AS wins FROM games GROUP BY opponent ORDER BY games DESC"):
        speeds = [dict(r) for r in conn.execute(
            "SELECT speed, COUNT(*) AS games, AVG(result) AS score FROM games "
            "WHERE opponent = ? GROUP BY speed ORDER BY games DESC", (row["opponent"],))]
        determinism, repeated = _determinism(conn, row["opponent"])
        result.append({**dict(row), "speeds": speeds, "determinism": determinism,
                       "repeated_positions": repeated})
    return result


def _opening_san(moves: str, plies: int) -> str:
    board = chess.Board()
    sans = []
    for uci in moves.split()[:plies]:
        move = chess.Move.from_uci(uci)
        sans.append(board.san(move))
        board.push(move)
    return " ".join(sans)


def opponent_detail(conn: sqlite3.Connection, opponent: str, settings: dict) -> dict:
    """Per speed/color summary, most played lines, and recent games of one opponent."""
    rows = conn.execute("SELECT * FROM games WHERE opponent = ? ORDER BY created_at DESC",
                        (opponent,)).fetchall()
    games = [scoring.GameRow(r["id"], r["result"], r["exit_ply"], r["n_plies"]) for r in rows]
    signals = scoring.game_signals(conn, games, settings)
    groups: dict[tuple[str, str], list[sqlite3.Row]] = {}
    lines: dict[tuple[str, str, str], list[sqlite3.Row]] = {}
    for row in rows:
        groups.setdefault((row["speed"], row["color"]), []).append(row)
        line = " ".join(row["moves"].split()[:LINE_PLIES])
        lines.setdefault((row["speed"], row["color"], line), []).append(row)
    summary = [{
        "speed": speed, "color": color, "games": len(group),
        "wins": sum(1 for r in group if r["result"] == 1.0),
        "draws": sum(1 for r in group if r["result"] == 0.5),
        "losses": sum(1 for r in group if r["result"] == 0.0),
        "signal": _score([signals[r["id"]] for r in group]),
        "avg_exit_ply": _score([float(r["exit_ply"]) for r in group]),
    } for (speed, color), group in sorted(groups.items())]
    top_lines = sorted(lines.items(), key=lambda item: len(item[1]), reverse=True)[:20]
    determinism, repeated = _determinism(conn, opponent)
    return {
        "opponent": opponent,
        "name": rows[0]["opponent_name"] if rows else opponent,
        "determinism": determinism, "repeated_positions": repeated, "summary": summary,
        "lines": [{"speed": s, "color": c, "moves": line.split(),
                   "san": _opening_san(line, LINE_PLIES), "games": len(group),
                   "score": _score([r["result"] for r in group]),
                   "signal": _score([signals[r["id"]] for r in group])}
                  for (s, c, line), group in top_lines],
        "games": [{"id": r["id"], "created_at": r["created_at"], "speed": r["speed"],
                   "color": r["color"], "result": r["result"], "status": r["status"],
                   "exit_ply": r["exit_ply"], "exit_source": r["exit_source"],
                   "signal": signals[r["id"]], "opening": _opening_san(r["moves"], 10)}
                  for r in rows[:RECENT_GAMES]],
    }


def _played_here(conn: sqlite3.Connection, key: int, opponent: str, speed: str,
                 board: chess.Board) -> list[dict]:
    """Moves actually played in this position, ours and the opponents', with counts."""
    rows = conn.execute(
        "SELECT gm.move, gm.by_us, g.opponent, g.speed, g.result FROM game_moves gm "
        "JOIN games g ON g.id = gm.game_id WHERE gm.key = ?", (key,)).fetchall()
    moves: dict[tuple[str, int], dict] = {}
    for row in rows:
        entry = moves.setdefault((row["move"], row["by_us"]), {
            "move": row["move"], "san": board.san(chess.Move.from_uci(row["move"]))
            if chess.Move.from_uci(row["move"]) in board.legal_moves else row["move"],
            "by_us": bool(row["by_us"]), "games": 0, "results": [], "opponent_games": 0})
        entry["games"] += 1
        entry["results"].append(row["result"])
        if row["opponent"] == opponent and (not speed or row["speed"] == speed):
            entry["opponent_games"] += 1
    played = []
    for entry in moves.values():
        results = entry.pop("results")
        played.append({**entry, "score": _score(results)})
    return sorted(played, key=lambda e: (not e["by_us"], -e["games"]))


def position(conn: sqlite3.Connection, moves: tuple[str, ...], opponent: str, speed: str,
             settings: dict, rng: random.Random) -> dict:
    """Everything the explorer shows for the position after ``moves``.

    Candidate statistics are computed for ``opponent`` at ``speed``; with an
    empty opponent every game counts as "other opponents", i.e. the pooled
    view used for an unknown opponent.
    """
    board = selector.board_from_moves(moves)
    analysis = selector.analyse_position(conn, board, opponent, speed, settings)
    playable = {v.move: v.opponent_post for v in analysis.candidates if v.enabled and v.playable}
    picks = stats.pick_probabilities(playable, rng)
    position_row = conn.execute("SELECT * FROM positions WHERE key = ?", (analysis.key,)).fetchone()
    queued = conn.execute("SELECT kind, status, error FROM analysis_queue WHERE target = ?",
                          (str(analysis.key),)).fetchall()
    candidates = []
    for view in analysis.candidates:
        move = chess.Move.from_uci(view.move)
        candidates.append({
            "move": view.move, "san": board.san(move), "tier": view.tier, "source": view.source,
            "enabled": view.enabled, "sf_expected": view.sf_expected, "sf_cp": view.sf_cp,
            "sf_mate": view.sf_mate, "pooled_n": view.pooled.n, "pooled_mean": view.pooled.mean,
            "pooled_post": view.pooled_post.mean, "opp_n": view.opponent.n,
            "opp_mean": view.opponent.mean, "opp_post": view.opponent_post.mean,
            "alpha": view.opponent_post.alpha, "beta": view.opponent_post.beta,
            "exhausted": view.exhausted, "playable": view.playable,
            "pick": picks.get(view.move, 0.0)})
    legal = sorted(({"move": m.uci(), "san": board.san(m)} for m in board.legal_moves),
                   key=lambda m: m["san"])
    return {
        "fen": board.fen(), "key": str(analysis.key), "ply": len(moves),
        "san_path": _opening_san(" ".join(moves), len(moves)),
        "turn": "white" if board.turn == chess.WHITE else "black",
        "in_horizon": len(moves) < int(settings["max_book_ply"]),
        "moves": list(moves), "candidates": candidates, "legal": legal,
        "tier_limit": analysis.tier_limit, "all_exhausted": analysis.all_exhausted,
        "widened": analysis.widened,
        "analysis": dict(position_row) | {"key": str(analysis.key)} if position_row else None,
        "jobs": [dict(r) for r in queued],
        "played": _played_here(conn, analysis.key, opponent, speed, board),
    }


def queue(conn: sqlite3.Connection) -> list[dict]:
    """Queued and running jobs first (by priority), then the latest finished ones."""
    return [dict(r) for r in conn.execute(
        "SELECT id, kind, target, fen, priority, status, error, requested_at, finished_at "
        "FROM analysis_queue ORDER BY CASE status WHEN 'running' THEN 0 WHEN 'queued' THEN 1 "
        "ELSE 2 END, CASE WHEN status IN ('running', 'queued') THEN priority ELSE 0 END, "
        "COALESCE(finished_at, requested_at) DESC LIMIT ?", (QUEUE_LIMIT,))]
