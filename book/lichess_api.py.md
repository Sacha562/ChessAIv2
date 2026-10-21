# Module: `book.lichess_api`

Minimal client for lichess's game export endpoint
(`GET /api/games/user/{username}`). It streams the bot's finished games as
NDJSON, oldest first, so [ingest.py](ingest.py.md) can store them incrementally
and resume after an interruption. Standard library only (`urllib`).

## Source File

- **Code:** [lichess_api.py](lichess_api.py)

## Objects / Interfaces

| Name | Meaning |
|------|---------|
| `API_ROOT` | `https://lichess.org` |
| `EXPORT_TIMEOUT_S` | Socket timeout for the streaming request (60 s). |
| `class RateLimited(Exception)` | Raised on HTTP 429. lichess asks clients to wait a full minute; the worker backs off 90 s ([worker.py.md](worker.py.md)). |

## Functions

### `export_games`

Yields each finished game of `username` created after `since_ms` (lichess
milliseconds) as a decoded dict. Query parameters: `moves=true` (SAN),
`finished=true`, `sort=dateAsc`; tags, clocks, evals and opening names are off.
Sends `Authorization: Bearer $LICHESS_TOKEN` when that environment variable is
set (higher rate limit). Without it, public games are exported anonymously,
roughly 25 games/s.

**Returns:** `Iterator[dict]`. Fields used downstream: `id`, `variant`,
`speed`, `rated`, `createdAt`, `status`, `winner`, `players.{white,black}.user.{id,name,title}`,
`players.*.rating`, `moves`, `initialFen`.

**Warnings:** raises [`RateLimited`](#objects--interfaces) on 429, and
`urllib.error.URLError` / `TimeoutError` on network failure. The caller decides
how to retry.
