"""lichess-bot engine class that asks the opening-book server before searching.

The LichessBotAPI compose stack bakes this file into the lichess-bot image at
``/lichess-bot/homemade.py``; ``config.yml`` selects it with engine protocol
``homemade`` and name ``BookedEngine``. It is a real UCI engine wrapper
(subclass of lichess-bot's ``UCIEngine``), so pondering, draw/resign logic and
move commentary keep working; it only short-circuits ``search`` when the book
server returns a move. Any server error falls back to the engine. Standard
library only, so it runs in the stock lichess-bot image. See
[homemade.py.md](homemade.py.md).
"""
from __future__ import annotations

import json
import logging
import urllib.request
from types import TracebackType
from typing import Any

import chess
import chess.engine

from lib import model
from lib.engine_wrapper import UCIEngine
from lib.lichess_types import COMMANDS_TYPE, MOVE, OPTIONS_GO_EGTB_TYPE
from lib.config import Configuration

logger = logging.getLogger(__name__)

DEFAULT_BOOK_SERVER = "http://book:8765"  # compose service name
DEFAULT_TIMEOUT_S = 1.0
NOTIFY_TIMEOUT_S = 2.0
START_FENS = (None, "startpos", chess.STARTING_FEN)


class BookedEngine(UCIEngine):
    """UCI engine whose moves come from the book server while it has one."""

    def __init__(self, commands: COMMANDS_TYPE, options: OPTIONS_GO_EGTB_TYPE, stderr: int | None,
                 draw_or_resign: Configuration, game: model.Game | None, debug: bool,
                 **popen_args: str) -> None:
        """Pop the book options, start the real engine, announce the game.

        ``homemade_options`` must contain ``BookEnginePath`` (the UCI binary
        inside the container); ``BookServer`` and ``BookTimeout`` are optional.
        Every other option is passed to the UCI engine unchanged.
        """
        engine_options = dict(options)
        self.book_server = str(engine_options.pop("BookServer", DEFAULT_BOOK_SERVER)).rstrip("/")
        self.book_timeout = float(engine_options.pop("BookTimeout", DEFAULT_TIMEOUT_S))
        engine_path = str(engine_options.pop("BookEnginePath"))
        self.game = game
        self.book_active = (game is not None and game.variant_name == "Standard"
                            and game.initial_fen in START_FENS)
        super().__init__([engine_path], engine_options, stderr, draw_or_resign, game, debug,
                         **popen_args)
        if self.book_active and game is not None:
            self._post("/api/shim/game_start", {"game_id": game.id}, NOTIFY_TIMEOUT_S)

    def _post(self, path: str, payload: dict, timeout: float) -> dict[str, Any] | None:
        """POST JSON to the book server; None (and a warning) on any failure."""
        request = urllib.request.Request(
            self.book_server + path, data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read())
        except Exception as error:  # the book must never break a game
            logger.warning("book server %s failed: %r", path, error)
            return None

    def _book_move(self, board: chess.Board) -> chess.Move | None:
        if not self.book_active or self.game is None:
            return None
        reply = self._post("/api/shim/select", {
            "game_id": self.game.id, "moves": [m.uci() for m in board.move_stack],
            "color": self.game.my_color, "opponent": self.game.opponent.name or "",
            "speed": self.game.speed or "unknown"}, self.book_timeout)
        if not reply or not reply.get("move"):
            return None
        move = chess.Move.from_uci(reply["move"])
        if move not in board.legal_moves:
            logger.warning("book server returned illegal move %s", reply["move"])
            return None
        logger.info("book move %s (%s)", reply["move"], reply.get("source"))
        return move

    def search(self, board: chess.Board, time_limit: chess.engine.Limit, ponder: bool,
               draw_offered: bool, root_moves: MOVE) -> chess.engine.PlayResult:
        """Play the book move if the server has one, otherwise search normally."""
        move = None if isinstance(root_moves, list) else self._book_move(board)
        if move is None:
            return super().search(board, time_limit, ponder, draw_offered, root_moves)
        self.engine.ping()  # a new command stops any ponder search on a stale position
        self.scores.append(chess.engine.PovScore(chess.engine.Mate(1), board.turn))
        return chess.engine.PlayResult(move, None)

    def __exit__(self, exc_type: type[BaseException] | None, exc_value: BaseException | None,
                 traceback: TracebackType | None) -> None:
        """Shut the engine down, then tell the book server the game is over.

        lichess-bot leaves this context once per game however it ended
        (result, abort, disconnect), so the server always unpauses analysis.
        """
        super().__exit__(exc_type, exc_value, traceback)
        if self.book_active and self.game is not None:
            self._post("/api/shim/game_end", {"game_id": self.game.id}, NOTIFY_TIMEOUT_S)
