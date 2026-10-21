# Frontend: `app.js`

The dashboard: a hash-routed single-page app in vanilla JavaScript (no build
step, no dependencies) over the JSON API in [server.py](../server.py.md#dashboard-endpoints).
It renders into `main#view` of [index.html](index.html.md) and is styled by
[style.css](style.css.md).

## Source File

- **Script:** [app.js](app.js)

## Routes / views

| Hash | View | API calls | Shows / does |
|------|------|-----------|--------------|
| `#/overview` (default) | `renderOverview` | `GET /api/overview`, `POST /api/ingest` | Stat tiles (games, opponents, score, analysed positions, candidates, manual candidates); per-speed table with a link to the pooled book; worker state, last ingest, per-kind queue counts, "Ingest now"; recent book decisions linked to lichess. Refreshes every 5 s. |
| `#/opponents` | `renderOpponents` | `GET /api/opponents` | Filterable table: games, score, per-speed chips, determinism %, wins (replayable), last played. |
| `#/opponent/<id>` | `renderOpponent` | `GET /api/opponents/<id>` | Per speed/color W/D/L, mean signal and book exit; most played 6-ply lines (open in the explorer); recent games with signal and exit ply. |
| `#/explore?moves=&opp=&speed=` | `renderExplore` | `GET /api/settings`, `GET /api/position`, `GET /api/opponents` (name suggestions) | Board + candidates for one position, opponent and speed. See below. |
| `#/queue` | `renderQueue` | `GET /api/queue` | Running/queued jobs, then recent finished ones with errors. Refreshes every 5 s. |
| `#/settings` | `renderSettings` | `GET /api/settings`, `GET /api/calibration`, `PUT /api/settings` | Editable table of every setting (checkbox for booleans; JSON for `tier_gaps`) with defaults; the eval-window calibration bar chart. |

### Explorer

- **Board.** Click a piece of the side to move, then a target square. Promotions
  default to a queen. The move is appended to the URL, so browser back/forward
  and links work. "Back", "Flip", and "Start" navigate; the SAN path is
  clickable.
- **Filters.** Opponent (empty = pooled view of all opponents) and speed.
- **Candidates table.** For each candidate:
  - **Identity:** move, tier, source.
  - **Stockfish:** score and expected score.
  - **Evidence:** pooled n and mean, opponent n and mean.
  - **Verdict:** opponent posterior (the hover shows the Beta parameters), Thompson
    pick probability, and status (`playable` / `locked tier` / `exhausted` / `disabled`).

  Actions:
  - **enable/disable**
  - **set tier:** this makes the candidate manual
  - **delete:** manual candidates only
- **Analysis panel.** Analysis depth and status; **Analyse now** / **Widen**
  queue a forced Stockfish job. **Add candidate** picks any legal move from a
  dropdown plus a tier and posts it as manual.
- **Moves played here.** Our and opponents' moves in this position, with game
  counts (overall and against the selected opponent) and our score.

## Helpers

- `esc`: HTML-escapes every server value inserted into markup. Opponent names come from lichess, so this blocks XSS.
- `pct`, `num`, `when`, `scoreClass`, `sfText`: formatting. `when` accepts seconds or lichess milliseconds.
- `api`: `fetch` wrapper that sends JSON and throws on non-2xx with the server's `detail`.
- `toast`: shows a transient message.
- `parseHash`, `exploreLink`: routing helpers.
- `setActiveNav`: highlights the active nav link.
- `updateBadge`: shows the worker state, current job kind and live games in the header. Polled every 10 s.
- `parseFenBoard`, `boardHtml`, `moveListHtml`, `candidateRows`, `playedRows`: explorer rendering.
- `wireExplorer`: attaches board and button handlers. After each change it re-routes to refresh.

## Behavior notes

- **Stale renders.** `routeSeq` is bumped on every navigation. A render whose
  fetch started before the latest navigation returns without painting, so a
  slow overview refresh cannot overwrite the explorer.
- **Auto-refresh.** Auto-refresh timers are cleared on every route change.
