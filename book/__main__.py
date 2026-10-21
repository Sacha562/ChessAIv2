"""Command line: ``python -m book serve | ingest | requeue | calibrate``.

``serve`` runs the dashboard + shim API with the background worker; ``ingest``
pulls new lichess games once without starting Stockfish; ``requeue`` rebuilds
the candidate queue from history; ``calibrate`` prints the eval-window
reliability table. See [__main__.py.md](__main__.py.md).
"""
from __future__ import annotations

import argparse
import json
import logging

from . import config, db, ingest, scoring


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m book", description=__doc__)
    parser.add_argument("--db", help=f"SQLite path (default {config.DEFAULT_DB_PATH})")
    parser.add_argument("--stockfish", help=f"Stockfish binary (default {config.DEFAULT_STOCKFISH})")
    sub = parser.add_subparsers(dest="command", required=True)
    serve = sub.add_parser("serve", help="run dashboard, shim API and worker")
    serve.add_argument("--host", help=f"bind address (default {config.DEFAULT_HOST})")
    serve.add_argument("--port", type=int, help=f"port (default {config.DEFAULT_PORT})")
    sub.add_parser("ingest", help="download new games once and exit")
    sub.add_parser("requeue", help="rebuild the candidate queue under the current visit policy")
    sub.add_parser("calibrate", help="print eval-window split-half reliability")
    return parser


def main() -> None:
    """Parse arguments and run the chosen command."""
    args = _parser().parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    static = config.static_config_from_env(args.db, args.stockfish, getattr(args, "host", None),
                                           getattr(args, "port", None))
    if args.command == "serve":
        import uvicorn

        from .server import create_app
        uvicorn.run(create_app(static), host=static.host, port=static.port, log_level="warning")
        return
    with db.session(static.db_path) as conn:
        settings = config.load_settings(conn)
        if args.command == "ingest":
            print(f"stored {ingest.ingest_new(conn, settings)} games")
        elif args.command == "requeue":
            print(f"queued {ingest.rebuild_candidate_queue(conn, settings)} positions")
        else:
            print(json.dumps(scoring.calibrate(conn, settings), indent=2))


if __name__ == "__main__":
    main()
