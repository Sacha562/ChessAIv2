# Module: `book.__main__`

Command-line entry point: `python -m book <command>` (run with the package
virtualenv, `book/.venv/Scripts/python`, from the repo root).

## Source File

- **Code:** [\_\_main\_\_.py](__main__.py)

## How to Build & Run

```bash
book/.venv/Scripts/python -m book serve
book/.venv/Scripts/python -m book serve --host 0.0.0.0 --port 8765
book/.venv/Scripts/python -m book ingest
book/.venv/Scripts/python -m book requeue
book/.venv/Scripts/python -m book calibrate
```

| Command | What it does |
|---------|--------------|
| `serve` | Runs the [FastAPI app](server.py.md) under uvicorn with the background [worker](worker.py.md): dashboard, shim API, Stockfish queue, periodic ingest. |
| `ingest` | [`ingest_new`](ingest.py.md#ingest_new) once; no Stockfish. Queued jobs run the next time `serve` runs. |
| `requeue` | [`rebuild_candidate_queue`](ingest.py.md#rebuild_candidate_queue): re-plans candidate analysis from history after changing `max_book_ply`, `candidate_always_ply` or `candidate_min_visits`. |
| `calibrate` | Prints [`calibrate`](scoring.py.md#calibrate) as JSON. |

Global options `--db` and `--stockfish`, and `serve`'s `--host` and `--port`,
override the defaults and environment variables described in
[config.py.md](config.py.md#static_config_from_env).

## Functions

### `_parser`

Builds the `argparse` parser with the four subcommands.

### `main`

Parses arguments, configures logging (INFO), builds the
[`StaticConfig`](config.py.md#class-staticconfig), and runs the command. `uvicorn`
and the server module are imported only for `serve`.
