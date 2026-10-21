"""Stockfish wrapper used for candidate generation and the eval signal.

Every score is reported as an *expected score* in [0, 1] (win + draw / 2) from
Stockfish's WDL output, because expected scores average sensibly and saturate
for decided positions, unlike centipawns. See [engine.py.md](engine.py.md).
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import chess
import chess.engine


@dataclass(frozen=True)
class LineEval:
    """Stockfish's verdict on one root move, from the side to move's view."""

    move: chess.Move
    expected: float
    cp: int | None
    mate: int | None
    depth: int


def expected_score(info: chess.engine.InfoDict, board: chess.Board, pov: chess.Color) -> float:
    """Expected score for ``pov`` from one analysis line.

    Uses the engine's own WDL (``UCI_ShowWDL``) when present, otherwise
    python-chess's Stockfish WDL model applied to the centipawn score.
    """
    wdl = info.get("wdl")
    if wdl is None:
        wdl = info["score"].wdl(model="sf", ply=board.ply())
    return wdl.pov(pov).expectation()


def _line_eval(info: chess.engine.InfoDict, board: chess.Board) -> LineEval:
    score = info["score"].pov(board.turn)
    return LineEval(move=info["pv"][0], expected=expected_score(info, board, board.turn),
                    cp=score.score(), mate=score.mate(), depth=int(info.get("depth", 0)))


class Stockfish:
    """One Stockfish process. Not thread-safe: the analysis worker owns it."""

    def __init__(self, path: Path, threads: int, hash_mb: int) -> None:
        self.engine = chess.engine.SimpleEngine.popen_uci(str(path))
        options = {"Threads": threads, "Hash": hash_mb}
        if "UCI_ShowWDL" in self.engine.options:
            options["UCI_ShowWDL"] = True
        self.engine.configure(options)

    def close(self) -> None:
        """Shut the process down."""
        self.engine.quit()

    def top_moves(self, board: chess.Board, depth: int, multipv: int) -> list[LineEval]:
        """The best ``multipv`` moves at fixed ``depth``, best first."""
        infos = self.engine.analyse(board, chess.engine.Limit(depth=depth), multipv=multipv)
        lines = [_line_eval(info, board) for info in infos if info.get("pv")]
        return sorted(lines, key=lambda line: line.expected, reverse=True)

    def eval_moves(self, board: chess.Board, moves: list[chess.Move], depth: int) -> list[LineEval]:
        """Evaluate exactly ``moves`` (searchmoves) at fixed ``depth``, best first."""
        if not moves:
            return []
        infos = self.engine.analyse(board, chess.engine.Limit(depth=depth),
                                    multipv=len(moves), root_moves=moves)
        lines = [_line_eval(info, board) for info in infos if info.get("pv")]
        return sorted(lines, key=lambda line: line.expected, reverse=True)

    def position_expected(self, board: chess.Board, nodes: int, pov: chess.Color) -> float:
        """Expected score for ``pov`` in ``board`` after a fixed-``nodes`` search."""
        info = self.engine.analyse(board, chess.engine.Limit(nodes=nodes))
        return expected_score(info, board, pov)
