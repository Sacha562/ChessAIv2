# Dockerfile (book service image)

Builds the `smikkelbot-book` image: the opening-book server
([\_\_init\_\_.py.md](__init__.py.md)) plus Stockfish, so the book runs in the same
Docker Compose stack as lichess-bot. The stack itself (the `book` and
`lichess-bot` services, and the `book-data` volume) is defined in the separate
`LichessBotAPI/docker-compose.yml`, which builds this file with
`context: ../ChessAIv2/book`.

## Source File

- **Config:** [Dockerfile](Dockerfile)
- **Build-context filter:** [.dockerignore](.dockerignore) excludes `.venv/`, `data/`, caches, `tests/`, and the `.md` companions.

## Keys / Settings

### Base image

- **Value:** `python:3.14-slim`
- **Description:** Matches the Python the package is developed and tested with.

### Stockfish

- **Value:** official `sf_19` asset `stockfish-linux-x86-64-universal.tar.gz`, `ADD --checksum=sha256:9defc0d4…611f` (the digest GitHub publishes for the asset), installed as `/usr/local/bin/stockfish`.
- **Description:** "universal" picks the best instruction set at runtime. To upgrade, change the URL and checksum together, then rebuild; the checksum change invalidates the cached layer.

### Package layout

- **Value:** `requirements.txt` is installed first (cached layer), then the directory is copied to `/app/book`. The container runs `python -m book serve` from `/app`.

### Environment

| Variable | Value | Effect ([config.py.md](config.py.md#static_config_from_env)) |
|----------|-------|--------|
| `BOOK_DB` | `/data/book.sqlite` | Database on the `/data` volume (named volume `book-data` in compose). |
| `BOOK_STOCKFISH` | `/usr/local/bin/stockfish` | Engine for the [worker](worker.py.md). |
| `BOOK_HOST` / `BOOK_PORT` | `0.0.0.0` / `8765` | Listen on all container interfaces. Compose publishes the port to `127.0.0.1` only; lichess-bot reaches the service as `http://book:8765`. |

### Healthcheck

- **Value:** `GET /api/settings` every 30 s.
- **Description:** Shows the container as healthy or unhealthy in `docker compose ps`. lichess-bot does not wait for it, because the shim falls back to the engine when the book is down.

## Notes

- SQLite stays on a Docker named volume, not a Windows bind mount. File locking
  over Docker Desktop's Windows bind mounts is unreliable, and only this
  container opens the database.
- Rebuild after changing the package: `docker compose up -d --build book`
  (from `LichessBotAPI/`). The database volume is kept.
