"""Background work: Stockfish analysis queue and periodic lichess ingest.

Two daemon threads run inside the server process. The analysis thread drains
``analysis_queue`` (candidate generation, widening, per-game eval signal) with
one Stockfish process and throttles itself while a lichess game is live so
Stockfish does not compete with the bot's engine for CPU: it either drops to
``stockfish_threads_in_game`` threads (default) or pauses entirely
(``pause_analysis_during_games``). The scheduler thread
ingests new games every ``ingest_interval_min`` minutes and shortly after each
game ends. See [worker.py.md](worker.py.md).
"""
from __future__ import annotations

import logging
import sqlite3
import threading
import time
import urllib.error

import chess
import chess.engine

from . import config, db, ingest, lichess_api, scoring
from .engine import LineEval, Stockfish

logger = logging.getLogger(__name__)

IDLE_WAIT_S = 10.0
PAUSED_WAIT_S = 5.0
SCHEDULER_TICK_S = 5.0
INGEST_AFTER_GAME_S = 10.0
RATE_LIMIT_BACKOFF_S = 90.0
STALE_GAME_S = 3 * 60 * 60


class Worker:
    """Owns the analysis and scheduler threads and their shared status."""

    def __init__(self, static: config.StaticConfig) -> None:
        self.static = static
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.wake_event = threading.Event()
        self.active_games: dict[str, float] = {}
        self.ingest_due_at = 0.0
        self.last_ingest_at = 0.0
        self.status: dict[str, object] = {"state": "starting", "job": None, "last_error": None,
                                          "last_ingest": None}
        self.stockfish: Stockfish | None = None
        self.stockfish_options: tuple[int, int] | None = None
        self.threads: list[threading.Thread] = []

    # -- lifecycle -----------------------------------------------------------

    def start(self) -> None:
        """Start both daemon threads."""
        for target, name in ((self._analysis_loop, "book-analysis"),
                             (self._scheduler_loop, "book-scheduler")):
            thread = threading.Thread(target=target, name=name, daemon=True)
            thread.start()
            self.threads.append(thread)

    def stop(self) -> None:
        """Ask both threads to exit and close Stockfish."""
        self.stop_event.set()
        self.wake_event.set()
        for thread in self.threads:
            thread.join(timeout=30.0)
        self._close_stockfish()

    # -- signals from the shim / dashboard ----------------------------------

    def game_started(self, game_id: str) -> None:
        """Mark a lichess game as running (pauses analysis if configured)."""
        with self.lock:
            self.active_games[game_id] = time.time()

    def game_ended(self, game_id: str) -> None:
        """Mark a game finished and schedule an ingest shortly after."""
        with self.lock:
            self.active_games.pop(game_id, None)
        self.request_ingest(INGEST_AFTER_GAME_S)
        self.wake_event.set()

    def request_ingest(self, delay_s: float = 0.0) -> None:
        """Run an ingest ``delay_s`` seconds from now (or sooner if already due)."""
        with self.lock:
            due = time.time() + delay_s
            self.ingest_due_at = due if self.ingest_due_at == 0.0 else min(self.ingest_due_at, due)

    def wake(self) -> None:
        """Wake the analysis thread early (new job queued)."""
        self.wake_event.set()

    def snapshot(self) -> dict[str, object]:
        """Copy of the status for the dashboard."""
        with self.lock:
            return {**self.status, "active_games": sorted(self.active_games),
                    "paused": self.status.get("state") == "paused"}

    # -- internals -----------------------------------------------------------

    def _set(self, **fields: object) -> None:
        with self.lock:
            self.status.update(fields)

    def _game_running(self) -> bool:
        cutoff = time.time() - STALE_GAME_S
        with self.lock:
            for game_id in [g for g, t in self.active_games.items() if t < cutoff]:
                del self.active_games[game_id]
            return bool(self.active_games)

    def _close_stockfish(self) -> None:
        if self.stockfish is not None:
            try:
                self.stockfish.close()
            except chess.engine.EngineError:
                logger.exception("closing Stockfish")
            self.stockfish = None

    def _ensure_stockfish(self, settings: dict, in_game: bool) -> Stockfish:
        """Start Stockfish, or restart it when the wanted threads/hash changed.

        While a game is live it runs with ``stockfish_threads_in_game`` threads
        so the bot's engine keeps most of the CPU.
        """
        threads = settings["stockfish_threads_in_game" if in_game else "stockfish_threads"]
        options = (int(threads), int(settings["stockfish_hash_mb"]))
        if self.stockfish is None or self.stockfish_options != options:
            self._close_stockfish()
            self.stockfish = Stockfish(self.static.stockfish_path, *options)
            self.stockfish_options = options
        return self.stockfish

    def _analysis_loop(self) -> None:
        while not self.stop_event.is_set():
            try:
                waited = self._analysis_step()
            except Exception as error:  # keep the thread alive; surface the error
                logger.exception("analysis step failed")
                self._set(state="error", last_error=repr(error))
                self._close_stockfish()
                waited = IDLE_WAIT_S
            if waited:
                self.wake_event.wait(waited)
                self.wake_event.clear()

    def _analysis_step(self) -> float:
        """Run at most one job. Returns seconds to wait before the next step (0 = none)."""
        with db.session(self.static.db_path) as conn:
            settings = config.load_settings(conn)
            in_game = self._game_running()
            if in_game and settings["pause_analysis_during_games"]:
                self._set(state="paused", job=None)
                return PAUSED_WAIT_S
            job = _claim_next(conn)
            if job is None:
                self._set(state="idle", job=None)
                return IDLE_WAIT_S
            self._set(state="analysing", job={"kind": job["kind"], "target": job["target"]})
            try:
                stockfish = self._ensure_stockfish(settings, in_game)
                run_job(conn, stockfish, job, settings)
                _finish(conn, job["id"], "done", None)
            except chess.engine.EngineTerminatedError as error:
                _finish(conn, job["id"], "failed", repr(error))
                self.stockfish = None
                raise
            except Exception as error:
                _finish(conn, job["id"], "failed", repr(error))
                raise
        return 0.0

    def _scheduler_loop(self) -> None:
        while not self.stop_event.wait(SCHEDULER_TICK_S):
            try:
                self._scheduler_step()
            except Exception as error:
                logger.exception("scheduler step failed")
                self._set(last_error=repr(error))

    def _scheduler_step(self) -> None:
        with db.session(self.static.db_path) as conn:
            settings = config.load_settings(conn)
            interval = 60.0 * float(settings["ingest_interval_min"])
            now = time.time()
            with self.lock:
                requested = 0.0 < self.ingest_due_at <= now
            if not requested and now - self.last_ingest_at < interval:
                return
            with self.lock:
                self.ingest_due_at = 0.0
            self.last_ingest_at = now
            try:
                stored = ingest.ingest_new(conn, settings)
                self._set(last_ingest={"at": int(now), "stored": stored, "error": None})
            except lichess_api.RateLimited:
                self.last_ingest_at = now - interval + RATE_LIMIT_BACKOFF_S
                self._set(last_ingest={"at": int(now), "stored": 0, "error": "rate limited"})
            except (urllib.error.URLError, TimeoutError) as error:
                self._set(last_ingest={"at": int(now), "stored": 0, "error": repr(error)})
        self.wake_event.set()


def _claim_next(conn: sqlite3.Connection) -> sqlite3.Row | None:
    """Mark the most urgent queued job as running and return it."""
    row = conn.execute("SELECT * FROM analysis_queue WHERE status = 'queued' "
                       "ORDER BY priority, requested_at LIMIT 1").fetchone()
    if row is None:
        return None
    conn.execute("UPDATE analysis_queue SET status = 'running' WHERE id = ?", (row["id"],))
    conn.commit()
    return row


def _finish(conn: sqlite3.Connection, job_id: int, status: str, error: str | None) -> None:
    conn.execute("UPDATE analysis_queue SET status = ?, error = ?, finished_at = ? WHERE id = ?",
                 (status, error, db.now(), job_id))
    conn.commit()


def reset_running(conn: sqlite3.Connection) -> None:
    """Re-queue jobs left 'running' by a previous process that died mid-job."""
    conn.execute("UPDATE analysis_queue SET status = 'queued' WHERE status = 'running'")
    conn.commit()


def run_job(conn: sqlite3.Connection, stockfish: Stockfish, job: sqlite3.Row, settings: dict) -> None:
    """Dispatch one queued job to its runner."""
    if job["kind"] == "signal":
        run_signal(conn, stockfish, job["target"], settings)
        return
    if job["kind"] in ("candidates", "widen"):
        run_candidates(conn, stockfish, int(job["target"]), job["fen"], settings,
                       widen=job["kind"] == "widen")
        return
    raise ValueError(f"unknown job kind {job['kind']}")


def _tier_for(gap: float, gaps: list[float], max_tier: int) -> int | None:
    """First tier whose gap bound covers ``gap``, limited to ``max_tier``."""
    for tier, bound in enumerate(gaps[:max_tier + 1]):
        if gap <= bound:
            return tier
    return None


def _store_line(conn: sqlite3.Connection, key: int, line: LineEval, tier: int | None,
                source: str) -> None:
    """Upsert one analysed move. Manual candidates keep their tier and source;
    engine/ours moves outside every allowed tier are kept but disabled."""
    enabled = 0 if tier is None else 1
    conn.execute(
        "INSERT INTO candidates(key, move, tier, source, sf_cp, sf_mate, sf_expected, sf_depth, "
        "enabled, added_at) VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(key, move) DO UPDATE SET "
        "  sf_cp = excluded.sf_cp, sf_mate = excluded.sf_mate, "
        "  sf_expected = excluded.sf_expected, sf_depth = excluded.sf_depth, "
        "  tier = CASE WHEN source = 'manual' THEN tier ELSE excluded.tier END, "
        "  enabled = CASE WHEN source = 'manual' THEN enabled ELSE excluded.enabled END",
        (key, line.move.uci(), 99 if tier is None else tier, source, line.cp, line.mate,
         line.expected, line.depth, enabled, db.now()))


def run_candidates(conn: sqlite3.Connection, stockfish: Stockfish, key: int, fen: str,
                   settings: dict, widen: bool) -> None:
    """Generate or refresh the candidates of one position.

    Runs MultiPV at fixed depth, then evaluates (searchmoves) every move we have
    played here and every existing candidate that MultiPV did not list. Moves
    are tiered by their expected-score gap to the best move; a normal run fills
    every tier but the last, a widen run (or any run on an already-widened
    position) is deeper, wider and also fills the last tier.
    """
    board = chess.Board(fen)
    row = conn.execute("SELECT widened FROM positions WHERE key = ?", (key,)).fetchone()
    widen = widen or bool(row and row["widened"])
    gaps = [float(g) for g in settings["tier_gaps"]]  # type: ignore[union-attr]
    max_tier = len(gaps) - 1 if widen else max(len(gaps) - 2, 0)
    depth = int(settings["analysis_depth"]) + (int(settings["widen_depth_bonus"]) if widen else 0)
    multipv = int(settings["widen_multipv"] if widen else settings["analysis_multipv"])
    lines = stockfish.top_moves(board, depth, multipv)
    listed = {line.move for line in lines}
    extra_ucis = {r["move"] for r in conn.execute(
        "SELECT DISTINCT move FROM game_moves WHERE key = ? AND by_us = 1 "
        "UNION SELECT move FROM candidates WHERE key = ?", (key, key))}
    extras = [m for m in (chess.Move.from_uci(u) for u in extra_ucis)
              if m in board.legal_moves and m not in listed]
    extra_lines = stockfish.eval_moves(board, extras, depth)
    best = max([line.expected for line in lines + extra_lines], default=0.5)
    for line in lines:
        _store_line(conn, key, line, _tier_for(best - line.expected, gaps, max_tier), "engine")
    for line in extra_lines:
        _store_line(conn, key, line, _tier_for(best - line.expected, gaps, max_tier), "ours")
    conn.execute("INSERT INTO positions(key, fen) VALUES(?, ?) ON CONFLICT(key) DO NOTHING",
                 (key, fen))
    conn.execute("UPDATE positions SET best_expected = ?, analysed_depth = ?, "
                 "analysed_multipv = ?, analysed_at = ?, widened = MAX(widened, ?) WHERE key = ?",
                 (best, depth, multipv, db.now(), int(widen), key))
    conn.commit()


def run_signal(conn: sqlite3.Connection, stockfish: Stockfish, game_id: str, settings: dict) -> None:
    """Evaluate the plies after one game's book exit for the eval signal."""
    game = conn.execute("SELECT * FROM games WHERE id = ?", (game_id,)).fetchone()
    if game is None:
        return
    color = chess.WHITE if game["color"] == "white" else chess.BLACK
    wanted = set(scoring.signal_plies(game["exit_ply"], game["n_plies"], settings))
    done = {r["ply"] for r in conn.execute("SELECT ply FROM signal_evals WHERE game_id = ?",
                                           (game_id,))}
    todo = wanted - done
    board = chess.Board()
    nodes = int(settings["signal_nodes"])
    for ply, uci in enumerate(game["moves"].split()):
        if not todo:
            break
        if ply in todo:
            expected = stockfish.position_expected(board, nodes, color)
            conn.execute("INSERT OR REPLACE INTO signal_evals(game_id, ply, expected) VALUES(?,?,?)",
                         (game_id, ply, expected))
            todo.discard(ply)
        board.push_uci(uci)
    conn.commit()
