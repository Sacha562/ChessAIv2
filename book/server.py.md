# Module: `book.server`

The FastAPI application. It serves three things:
- the **shim API** called from the lichess-bot container
  ([shim/homemade.py.md](shim/homemade.py.md))
- the **dashboard API**
- the dashboard's **static files** ([static/index.html.md](static/index.html.md))

Every request opens its own SQLite connection ([`db.session`](db.py.md#session)).
Synchronous handlers run on FastAPI's thread pool, so a slow dashboard query
never blocks a shim request. The app owns one [`Worker`](worker.py.md#class-worker),
started and stopped by the lifespan handler; jobs left `running` by a crash are
re-queued at start.

There is **no authentication**. Run directly, it binds to `127.0.0.1`. In the
compose stack it listens on `0.0.0.0` inside the container, but the port is
published to `127.0.0.1:8765` only ([Dockerfile.md](Dockerfile.md)).

## Source File

- **Code:** [server.py](server.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md) (`ServerTests`)

## How to Build & Run

`book/.venv/Scripts/python -m book serve` ([\_\_main\_\_.py.md](__main__.py.md)),
then open <http://127.0.0.1:8765>.

## Objects / Interfaces

Pydantic request bodies:

| Model | Fields | Used by |
|-------|--------|---------|
| `SelectBody` | `game_id`, `moves` (UCI list), `color` (`white`/`black`), `opponent`, `speed` | `POST /api/shim/select` |
| `GameBody` | `game_id` | `POST /api/shim/game_start`, `/game_end` |
| `CandidateBody` | `moves`, `move` (UCI or SAN), `tier` (0–9, default 0) | `POST /api/candidates` |
| `CandidatePatch` | `moves`, `move`, optional `enabled`, optional `tier` (0–9) | `PATCH /api/candidates` |
| `CandidateRef` | `moves`, `move` | `DELETE /api/candidates` |
| `AnalyseBody` | `moves`, `widen` | `POST /api/analyse` |

Candidates are always identified by the move list of their position, never by
key, because the browser cannot hold 64-bit keys.

### Shim endpoints

| Method | Path | Effect |
|--------|------|--------|
| POST | `/api/shim/select` | [`selector.choose`](selector.py.md#choose); returns `{move, source, detail}`. The opponent name is lowercased. Wakes the worker (a miss may have queued a job). |
| POST | `/api/shim/game_start` | [`Worker.game_started`](worker.py.md#workergame_started--workergame_ended): analysis pauses. |
| POST | `/api/shim/game_end` | [`Worker.game_ended`](worker.py.md#workergame_started--workergame_ended): analysis resumes; ingest in 10 s. |

### Dashboard endpoints

| Method | Path | Returns / effect |
|--------|------|------------------|
| GET | `/api/overview` | [`reports.overview`](reports.py.md#overview) + `worker`. |
| GET | `/api/opponents` | [`reports.opponents`](reports.py.md#opponents). |
| GET | `/api/opponents/{opponent}` | [`reports.opponent_detail`](reports.py.md#opponent_detail). |
| GET | `/api/position?moves=a,b&opponent=&speed=` | [`reports.position`](reports.py.md#position); 400 on illegal moves. |
| GET | `/api/queue` | `{jobs, worker}`. |
| GET | `/api/settings` | `{settings, defaults, speeds}`. |
| GET | `/api/calibration` | [`scoring.calibrate`](scoring.py.md#calibrate). |
| PUT | `/api/settings` | Body: `{name: value}`; validated by [`save_settings`](config.py.md#save_settings); 400 on a bad name or value. |
| POST | `/api/candidates` | Adds or overrides a candidate as `manual` (enabled, given tier); queues a forced re-analysis of the position at priority 1 so Stockfish scores it. Returns `{move, san}`. |
| PATCH | `/api/candidates` | Sets `enabled` and/or `tier`. Setting a tier also marks the candidate `manual`, so re-analysis no longer changes it. |
| DELETE | `/api/candidates` | Deletes a `manual` candidate; 400 for other sources (disable those instead). |
| POST | `/api/analyse` | Forces a `candidates` or `widen` job for the position (priority 1). |
| POST | `/api/ingest` | Ingest now. |
| GET | `/` and `/static/*` | Dashboard files from `book/static/`. |

## Functions

### `create_app`

Builds the app, the [`Worker`](worker.py.md#class-worker), and a shared
`random.Random` for Thompson draws. With `start_worker=False` (tests) the
background threads are not started; jobs queue up but nothing runs.

**Returns:** `FastAPI`. `app.state.worker` exposes the worker.

### `_board` / `_parse_move` / `_moves_param`

Request helpers:
- `_board` replays a move list, answering 400 on an illegal move.
- `_parse_move` accepts UCI or SAN, answering 400 if the move is not legal.
- `_moves_param` splits the comma-separated `moves` query parameter.
