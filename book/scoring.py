"""The per-game learning signal and its calibration.

A game's signal ``s`` in [0, 1] blends its result with Stockfish's average
expected score over a window of plies after the game left the book:
``s = w * result + (1 - w) * eval``. Per-ply evals are stored once
(``signal_evals``), so changing the window or the blend weight never needs
Stockfish again. See [scoring.py.md](scoring.py.md).
"""
from __future__ import annotations

import math
import sqlite3
from dataclasses import dataclass

CALIBRATION_WIDTH_PLIES = 20
CALIBRATION_STEP_PLIES = 4
CALIBRATION_MIN_GROUP = 4
CALIBRATION_MIN_GROUPS = 3
CALIBRATION_LINE_PLIES = 8


@dataclass(frozen=True)
class GameRow:
    """The fields of a stored game the signal needs."""

    id: str
    result: float
    exit_ply: int
    n_plies: int


def signal_plies(exit_ply: int, n_plies: int, settings: dict) -> list[int]:
    """Plies the worker evaluates for one game: every ``signal_stride`` plies
    from the exit up to ``signal_max_offset_plies`` past it, inside the game."""
    stride = int(settings["signal_stride"])
    last = exit_ply + int(settings["signal_max_offset_plies"])
    return [ply for ply in range(exit_ply, last + 1, stride) if ply < n_plies]


def eval_signal(game: GameRow, evals: dict[int, float], start_off: int, end_off: int,
                stride: int) -> float | None:
    """Mean expected score over plies ``exit + start_off .. exit + end_off``.

    Plies past the end of the game count as the game result. Returns None when
    an in-game ply of the window has not been evaluated yet.
    """
    values: list[float] = []
    for ply in range(game.exit_ply + start_off, game.exit_ply + end_off + 1, stride):
        if ply >= game.n_plies:
            values.append(game.result)
        elif ply in evals:
            values.append(evals[ply])
        else:
            return None
    return sum(values) / len(values) if values else None


def blend(result: float, evaluation: float | None, result_weight: float) -> float:
    """Combine result and eval signal; falls back to the result alone."""
    if evaluation is None:
        return result
    return result_weight * result + (1.0 - result_weight) * evaluation


def window_offsets(settings: dict) -> tuple[int, int]:
    """The configured window as (start, end) ply offsets from the book exit."""
    return 2 * int(settings["window_start_moves"]), 2 * int(settings["window_end_moves"])


def load_evals(conn: sqlite3.Connection, game_ids: list[str]) -> dict[str, dict[int, float]]:
    """Per-ply expected scores for ``game_ids``, keyed game id -> ply -> score."""
    evals: dict[str, dict[int, float]] = {gid: {} for gid in game_ids}
    for start in range(0, len(game_ids), 500):
        chunk = game_ids[start:start + 500]
        marks = ",".join("?" * len(chunk))
        for row in conn.execute(f"SELECT game_id, ply, expected FROM signal_evals "
                                f"WHERE game_id IN ({marks})", chunk):
            evals[row["game_id"]][row["ply"]] = row["expected"]
    return evals


def game_signals(conn: sqlite3.Connection, games: list[GameRow], settings: dict) -> dict[str, float]:
    """The blended signal ``s`` of each game, keyed by game id."""
    evals = load_evals(conn, [g.id for g in games])
    start_off, end_off = window_offsets(settings)
    stride = int(settings["signal_stride"])
    weight = float(settings["result_weight"])
    return {g.id: blend(g.result, eval_signal(g, evals[g.id], start_off, end_off, stride), weight)
            for g in games}


def _pearson(xs: list[float], ys: list[float]) -> float | None:
    n = len(xs)
    mean_x, mean_y = sum(xs) / n, sum(ys) / n
    cov = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    var_x = sum((x - mean_x) ** 2 for x in xs)
    var_y = sum((y - mean_y) ** 2 for y in ys)
    if var_x <= 0.0 or var_y <= 0.0:
        return None
    return cov / math.sqrt(var_x * var_y)


def _split_half(groups: list[list[float | None]]) -> tuple[float | None, int]:
    """Correlation between odd- and even-indexed halves of each group's signals."""
    odd_means, even_means = [], []
    for values in groups:
        odd = [v for v in values[0::2] if v is not None]
        even = [v for v in values[1::2] if v is not None]
        if odd and even:
            odd_means.append(sum(odd) / len(odd))
            even_means.append(sum(even) / len(even))
    if len(odd_means) < CALIBRATION_MIN_GROUPS:
        return None, len(odd_means)
    return _pearson(odd_means, even_means), len(odd_means)


def calibrate(conn: sqlite3.Connection, settings: dict) -> dict:
    """Split-half reliability of the signal for each candidate eval window.

    Games are grouped by (opponent, speed, color, first plies of the line);
    groups with at least CALIBRATION_MIN_GROUP games are split into odd and
    even games by date, and the Pearson correlation of the two halves' mean
    signal is computed across groups. A higher correlation means the window
    ranks lines more reproducibly. The pure game result is reported as a
    baseline row with ``start_moves = None``.
    """
    rows = conn.execute("SELECT id, opponent, speed, color, moves, result, exit_ply, n_plies "
                        "FROM games ORDER BY created_at").fetchall()
    grouped: dict[tuple, list[GameRow]] = {}
    for row in rows:
        line = " ".join(row["moves"].split()[:CALIBRATION_LINE_PLIES])
        key = (row["opponent"], row["speed"], row["color"], line)
        grouped.setdefault(key, []).append(
            GameRow(row["id"], row["result"], row["exit_ply"], row["n_plies"]))
    groups = [games for games in grouped.values() if len(games) >= CALIBRATION_MIN_GROUP]
    evals = load_evals(conn, [g.id for games in groups for g in games])
    stride = int(settings["signal_stride"])
    baseline, n_groups = _split_half([[g.result for g in games] for games in groups])
    windows = [{"start_moves": None, "end_moves": None, "r": baseline, "groups": n_groups}]
    last_start = int(settings["signal_max_offset_plies"]) - CALIBRATION_WIDTH_PLIES
    for start in range(0, last_start + 1, CALIBRATION_STEP_PLIES):
        end = start + CALIBRATION_WIDTH_PLIES
        signals = [[eval_signal(g, evals[g.id], start, end, stride) for g in games]
                   for games in groups]
        r, n_groups = _split_half(signals)
        windows.append({"start_moves": start // 2, "end_moves": end // 2, "r": r,
                        "groups": n_groups})
    return {"eligible_groups": len(groups), "windows": windows}
