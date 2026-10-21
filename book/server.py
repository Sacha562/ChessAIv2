"""FastAPI app: the lichess-bot shim API, the dashboard API, and static files.

Shim endpoints (``/api/shim/*``) are called from the lichess-bot Docker
container through ``host.docker.internal``; dashboard endpoints serve the
single-page app in ``static/``. All state lives in SQLite; each request opens
its own connection. See [server.py.md](server.py.md).
"""
from __future__ import annotations

import random
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

import chess
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import config, db, ingest, reports, scoring, selector
from .worker import Worker, reset_running

STATIC_DIR = config.PACKAGE_DIR / "static"
MANUAL_TIER_MAX = 9


class SelectBody(BaseModel):
    """A move request from the shim. ``moves`` are UCI from the start position."""

    game_id: str
    moves: list[str]
    color: str
    opponent: str
    speed: str


class GameBody(BaseModel):
    """Start/end notification from the shim."""

    game_id: str


class CandidateBody(BaseModel):
    """Add a manual candidate in the position after ``moves`` (UCI or SAN)."""

    moves: list[str]
    move: str
    tier: int = Field(default=0, ge=0, le=MANUAL_TIER_MAX)


class CandidatePatch(BaseModel):
    """Enable/disable a candidate or change its tier."""

    moves: list[str]
    move: str
    enabled: bool | None = None
    tier: int | None = Field(default=None, ge=0, le=MANUAL_TIER_MAX)


class CandidateRef(BaseModel):
    """Identify one candidate by the position's move list and the move."""

    moves: list[str]
    move: str


class AnalyseBody(BaseModel):
    """Force (re-)analysis of the position after ``moves``."""

    moves: list[str]
    widen: bool = False


def _board(moves: list[str]) -> chess.Board:
    try:
        return selector.board_from_moves(tuple(moves))
    except ValueError as error:
        raise HTTPException(status_code=400, detail=str(error)) from error


def _parse_move(board: chess.Board, text: str) -> chess.Move:
    text = text.strip()
    try:
        move = chess.Move.from_uci(text)
        if move in board.legal_moves:
            return move
    except ValueError:
        pass
    try:
        return board.parse_san(text)
    except ValueError as error:
        raise HTTPException(status_code=400, detail=f"not a legal move: {text}") from error


def _moves_param(moves: str) -> tuple[str, ...]:
    return tuple(m for m in moves.split(",") if m)


def create_app(static: config.StaticConfig, start_worker: bool = True) -> FastAPI:
    """Build the app. ``start_worker=False`` skips background threads (tests)."""
    worker = Worker(static)
    rng = random.Random()

    @asynccontextmanager
    async def lifespan(_: FastAPI) -> AsyncIterator[None]:
        with db.session(static.db_path) as conn:
            reset_running(conn)
        if start_worker:
            worker.start()
        yield
        if start_worker:
            worker.stop()

    app = FastAPI(title="SmikkelBot opening book", lifespan=lifespan)
    app.state.worker = worker

    # -- shim -----------------------------------------------------------------

    @app.post("/api/shim/select")
    def shim_select(body: SelectBody) -> dict:
        request = selector.Request(
            game_id=body.game_id, moves=tuple(body.moves),
            color=chess.WHITE if body.color == "white" else chess.BLACK,
            opponent=body.opponent.lower(), speed=body.speed)
        with db.session(static.db_path) as conn:
            decision = selector.choose(conn, request, config.load_settings(conn), rng)
        worker.wake()
        return {"move": decision.move, "source": decision.source, "detail": decision.detail}

    @app.post("/api/shim/game_start")
    def shim_game_start(body: GameBody) -> dict:
        worker.game_started(body.game_id)
        return {"ok": True}

    @app.post("/api/shim/game_end")
    def shim_game_end(body: GameBody) -> dict:
        worker.game_ended(body.game_id)
        return {"ok": True}

    # -- dashboard: read ------------------------------------------------------

    @app.get("/api/overview")
    def get_overview() -> dict:
        with db.session(static.db_path) as conn:
            return reports.overview(conn) | {"worker": worker.snapshot()}

    @app.get("/api/opponents")
    def get_opponents() -> list[dict]:
        with db.session(static.db_path) as conn:
            return reports.opponents(conn)

    @app.get("/api/opponents/{opponent}")
    def get_opponent(opponent: str) -> dict:
        with db.session(static.db_path) as conn:
            return reports.opponent_detail(conn, opponent.lower(), config.load_settings(conn))

    @app.get("/api/position")
    def get_position(moves: str = "", opponent: str = "", speed: str = "blitz") -> dict:
        try:
            parsed = _moves_param(moves)
            selector.board_from_moves(parsed)
        except ValueError as error:
            raise HTTPException(status_code=400, detail=str(error)) from error
        with db.session(static.db_path) as conn:
            return reports.position(conn, parsed, opponent.lower(), speed,
                                    config.load_settings(conn), rng)

    @app.get("/api/queue")
    def get_queue() -> dict:
        with db.session(static.db_path) as conn:
            return {"jobs": reports.queue(conn), "worker": worker.snapshot()}

    @app.get("/api/settings")
    def get_settings() -> dict:
        with db.session(static.db_path) as conn:
            return {"settings": config.load_settings(conn), "defaults": config.DEFAULT_SETTINGS,
                    "speeds": list(ingest.SPEED_ORDER)}

    @app.get("/api/calibration")
    def get_calibration() -> dict:
        with db.session(static.db_path) as conn:
            return scoring.calibrate(conn, config.load_settings(conn))

    # -- dashboard: write -----------------------------------------------------

    @app.put("/api/settings")
    def put_settings(updates: dict) -> dict:
        try:
            with db.session(static.db_path) as conn:
                return {"settings": config.save_settings(conn, updates)}
        except (KeyError, ValueError, TypeError) as error:
            raise HTTPException(status_code=400, detail=f"bad setting: {error}") from error

    @app.post("/api/candidates")
    def add_candidate(body: CandidateBody) -> dict:
        board = _board(body.moves)
        move = _parse_move(board, body.move)
        key = db.position_key(board)
        with db.session(static.db_path) as conn:
            conn.execute("INSERT OR IGNORE INTO positions(key, fen) VALUES(?, ?)", (key, board.fen()))
            conn.execute(
                "INSERT INTO candidates(key, move, tier, source, enabled, added_at) "
                "VALUES(?, ?, ?, 'manual', 1, ?) ON CONFLICT(key, move) DO UPDATE SET "
                "source = 'manual', tier = excluded.tier, enabled = 1",
                (key, move.uci(), body.tier, db.now()))
            db.enqueue(conn, "candidates", str(key), ingest.PRIORITY_MANUAL, fen=board.fen(),
                       force=True)
            conn.commit()
        worker.wake()
        return {"move": move.uci(), "san": board.san(move)}

    @app.patch("/api/candidates")
    def patch_candidate(body: CandidatePatch) -> dict:
        key = db.position_key(_board(body.moves))
        with db.session(static.db_path) as conn:
            if body.enabled is not None:
                conn.execute("UPDATE candidates SET enabled = ? WHERE key = ? AND move = ?",
                             (int(body.enabled), key, body.move))
            if body.tier is not None:
                conn.execute("UPDATE candidates SET tier = ?, source = 'manual' "
                             "WHERE key = ? AND move = ?", (body.tier, key, body.move))
            conn.commit()
        return {"ok": True}

    @app.delete("/api/candidates")
    def delete_candidate(body: CandidateRef) -> dict:
        key = db.position_key(_board(body.moves))
        with db.session(static.db_path) as conn:
            deleted = conn.execute("DELETE FROM candidates WHERE key = ? AND move = ? "
                                   "AND source = 'manual'", (key, body.move)).rowcount
            conn.commit()
        if not deleted:
            raise HTTPException(status_code=400, detail="only manual candidates can be deleted")
        return {"ok": True}

    @app.post("/api/analyse")
    def analyse(body: AnalyseBody) -> dict:
        board = _board(body.moves)
        key = db.position_key(board)
        with db.session(static.db_path) as conn:
            conn.execute("INSERT OR IGNORE INTO positions(key, fen) VALUES(?, ?)", (key, board.fen()))
            db.enqueue(conn, "widen" if body.widen else "candidates", str(key),
                       ingest.PRIORITY_MANUAL, fen=board.fen(), force=True)
            conn.commit()
        worker.wake()
        return {"ok": True}

    @app.post("/api/ingest")
    def trigger_ingest() -> dict:
        worker.request_ingest(0.0)
        return {"ok": True}

    # -- static ---------------------------------------------------------------

    @app.get("/")
    def index() -> FileResponse:
        return FileResponse(STATIC_DIR / "index.html")

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
    return app
