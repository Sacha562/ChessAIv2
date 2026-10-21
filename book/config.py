"""Static paths and tunable settings for the opening book.

Static values (database path, Stockfish binary, bind address) come from the
command line or environment and never change while the server runs. Tunable
values live in the ``settings`` table so the dashboard can edit them; the
defaults below fill any key the table does not hold. See
[config.py.md](config.py.md).
"""
from __future__ import annotations

import json
import os
import sqlite3
from dataclasses import dataclass
from pathlib import Path

PACKAGE_DIR = Path(__file__).resolve().parent
DEFAULT_DB_PATH = PACKAGE_DIR / "data" / "book.sqlite"
DEFAULT_STOCKFISH = Path.home() / "Documents" / "programs" / "stockfish" / "stockfish-18-bmi2.exe"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765

# Every tunable, with its default. The dashboard edits these through the
# settings table; types are enforced by coerce_setting.
DEFAULT_SETTINGS: dict[str, object] = {
    "account": "SmikkelBot",
    # Book shape
    "max_book_ply": 30,  # 15 full moves
    # Learning signal: s = result_weight * result + (1 - result_weight) * eval
    "result_weight": 0.5,
    "window_start_moves": 10,
    "window_end_moves": 20,
    # Posterior shrinkage (pseudo-games)
    "k_stockfish": 4.0,
    "k_pool": 5.0,
    "other_speed_weight": 0.5,
    # Tier unlocking
    "exhaust_threshold": 0.40,
    "exhaust_min_games": 3,
    "tier_gaps": [0.03, 0.07, 0.12, 0.18],
    # Candidate analysis
    "candidate_always_ply": 8,
    "candidate_min_visits": 2,
    "analysis_depth": 20,
    "analysis_multipv": 8,
    "widen_depth_bonus": 4,
    "widen_multipv": 16,
    # Per-game eval signal
    "signal_nodes": 1_000_000,
    "signal_stride": 2,
    "signal_max_offset_plies": 60,
    # Replay of won games
    "replay_enabled": True,
    "replay_min_result": 1.0,
    "replay_same_speed_only": True,
    # Scheduling
    "ingest_interval_min": 10,
    "pause_analysis_during_games": False,
    "stockfish_threads": 8,
    "stockfish_threads_in_game": 4,
    "stockfish_hash_mb": 256,
}


@dataclass(frozen=True)
class StaticConfig:
    """Paths and network settings fixed for the server's lifetime."""

    db_path: Path
    stockfish_path: Path
    host: str
    port: int


def static_config_from_env(db_path: str | None = None, stockfish_path: str | None = None,
                           host: str | None = None, port: int | None = None) -> StaticConfig:
    """Build the static config: explicit argument, else env var, else default."""
    return StaticConfig(
        db_path=Path(db_path or os.environ.get("BOOK_DB") or DEFAULT_DB_PATH),
        stockfish_path=Path(stockfish_path or os.environ.get("BOOK_STOCKFISH") or DEFAULT_STOCKFISH),
        host=host or os.environ.get("BOOK_HOST") or DEFAULT_HOST,
        port=int(port or os.environ.get("BOOK_PORT") or DEFAULT_PORT),
    )


def coerce_setting(name: str, value: object) -> object:
    """Convert ``value`` to the type of the default for ``name``.

    Raises KeyError for an unknown name and ValueError for a value that cannot
    be converted, so the dashboard cannot store a malformed setting.
    """
    default = DEFAULT_SETTINGS[name]
    if isinstance(default, bool):
        if isinstance(value, str):
            return value.strip().lower() in ("1", "true", "yes", "on")
        return bool(value)
    if isinstance(default, int):
        return int(value)  # type: ignore[arg-type]
    if isinstance(default, float):
        return float(value)  # type: ignore[arg-type]
    if isinstance(default, list):
        items = json.loads(value) if isinstance(value, str) else value
        gaps = sorted(float(x) for x in items)  # type: ignore[union-attr]
        if not gaps or gaps[0] <= 0.0:
            raise ValueError("tier_gaps must be positive")
        return gaps
    return str(value)


def load_settings(conn: sqlite3.Connection) -> dict[str, object]:
    """Return every setting: stored overrides on top of the defaults."""
    settings = dict(DEFAULT_SETTINGS)
    for name, raw in conn.execute("SELECT name, value FROM settings"):
        if name in settings:
            settings[name] = coerce_setting(name, json.loads(raw))
    return settings


def save_settings(conn: sqlite3.Connection, updates: dict[str, object]) -> dict[str, object]:
    """Validate and store ``updates``; return the full merged settings."""
    for name, value in updates.items():
        coerced = coerce_setting(name, value)
        conn.execute("INSERT INTO settings(name, value) VALUES(?, ?) "
                     "ON CONFLICT(name) DO UPDATE SET value = excluded.value",
                     (name, json.dumps(coerced)))
    conn.commit()
    return load_settings(conn)
