# Module: `book.worker`

Background work inside the server process ([server.py.md](server.py.md)): two
daemon threads.

- **Analysis thread.** Drains `analysis_queue` ([db.py.md](db.py.md)) in
  priority order with one Stockfish process ([engine.py.md](engine.py.md)).
  Jobs:
  - `candidates`: generate or refresh a position's book moves
  - `widen`: the same, deeper and wider
  - `signal`: evaluate a game's post-book plies

  It **throttles while a lichess game is live**: the shim announces start and
  end, and a game silent for 3 h is dropped. By default Stockfish keeps running
  with `stockfish_threads_in_game` (4) threads instead of `stockfish_threads`
  (8). With `pause_analysis_during_games` it stops entirely. Pausing would
  starve the queue, because matchmaking keeps the bot in games most of the
  time.
- **Scheduler thread.** Ingests new games ([ingest.py.md](ingest.py.md)) at
  start-up, every `ingest_interval_min` minutes, and 10 s after a game ends.

## Source File

- **Code:** [worker.py](worker.py)

## Objects / Interfaces

### Timing constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `IDLE_WAIT_S` | `10` | Sleep when the queue is empty or after an error (wakes early on new jobs). |
| `PAUSED_WAIT_S` | `5` | Poll interval while a game is live. |
| `SCHEDULER_TICK_S` | `5` | Scheduler poll interval. |
| `INGEST_AFTER_GAME_S` | `10` | Delay before ingesting a just-finished game (lichess export lag). |
| `RATE_LIMIT_BACKOFF_S` | `90` | Retry delay after HTTP 429. |
| `STALE_GAME_S` | `10800` | A live game with no end notice for 3 h is assumed over. |

### `class Worker`

Owns both threads, the Stockfish process, the set of live games, and a status
dict shown on the dashboard.

| Field | Type | Description |
|-------|------|-------------|
| `static` | [`StaticConfig`](config.py.md#class-staticconfig) | Database and Stockfish paths. |
| `lock` | `threading.Lock` | Guards `active_games`, `ingest_due_at`, `status`. |
| `active_games` | `dict[str, float]` | Live game id → start time. |
| `ingest_due_at` | `float` | Requested ingest time (0 = none). |
| `status` | `dict` | `state` (`starting`/`idle`/`analysing`/`paused`/`error`), current `job`, `last_error`, `last_ingest`. |
| `stockfish` | `Stockfish \| None` | Started lazily; restarted when the wanted threads/hash change (including entering or leaving a live game) or after a crash. |

## Functions

### `Worker.start` / `Worker.stop`

Start both daemon threads / signal them to stop, join (30 s each), and quit
Stockfish.

### `Worker.game_started` / `Worker.game_ended`

Called by the shim endpoints. `game_ended` also schedules an ingest in 10 s
and wakes the analysis thread.

### `Worker.request_ingest`

Schedules an ingest `delay_s` from now, keeping an earlier pending time if
one exists.

### `Worker.wake`

Wakes the analysis thread (after a job is queued from a request).

### `Worker.snapshot`

**Returns:** a copy of `status`, plus `active_games` (sorted ids) and `paused`.

### `Worker._analysis_step`

One iteration:
1. Load settings and check for a live game.
2. If a game is live and `pause_analysis_during_games` is set, return 5 s.
   Otherwise `_ensure_stockfish` picks the in-game or normal thread count.
3. Claim the most urgent `queued` job (lowest priority, then oldest), marking it
   `running`.
4. Run it, then mark it `done`.

A failing job is marked `failed` with its error; the exception is logged,
`state` becomes `error`, and Stockfish is restarted before the next job.

**Returns:** seconds to wait (0 to continue immediately).

### `Worker._scheduler_step`

Ingests when a requested time has passed or the interval has elapsed. On
`RateLimited` it retries after 90 s. Network errors are recorded in
`last_ingest` and retried at the next interval.

### `reset_running`

Re-queues jobs left `running` by a process that died mid-job. Called once at
server start.

### `run_job`

Dispatches a claimed row to [`run_signal`](#run_signal) or
[`run_candidates`](#run_candidates) (`widen=True` for `widen` jobs).

**Warnings:** raises `ValueError` on an unknown kind.

### `run_candidates`

Generates or refreshes the candidates of one position:

1. **Mode.** Widen if requested **or** if the position was ever widened; a
   widened position always stays widened.
2. **Search.** MultiPV `analysis_multipv` (normal) or `widen_multipv` (widen)
   at `analysis_depth` (`+ widen_depth_bonus` when widening).
3. **Extras.** Every move we have played here, and every existing candidate
   that MultiPV did not list, is evaluated with searchmoves at the same depth.
   This gives our own engine's choices (`source = 'ours'`) and manual moves a
   comparable score.
4. **Tiering.** For each move, gap = best expected score − its expected score.
   The tier is the first index of `tier_gaps` whose bound covers the gap.
   - Normal runs may fill every tier but the last.
   - Widen runs may fill the last tier too.
   - Moves beyond every allowed tier are stored with tier 99 and `enabled = 0`,
     so the dashboard can show them as rejected.
5. **Manual candidates** keep their tier and enabled flag; only their Stockfish
   fields update.
6. **Bookkeeping.** Updates `positions`: `best_expected`, depth, MultiPV,
   `analysed_at`, `widened`. Commits.

**Warnings:**
- A user-disabled *engine* candidate is re-enabled by the next re-analysis if it
  is still within a tier. To keep it out permanently, set its tier on the
  dashboard, which makes it manual.
- Changing `tier_gaps` does not re-tier existing candidates until each position
  is re-analysed ("Analyse now").

### `run_signal`

For one game, evaluates every ply from [`signal_plies`](scoring.py.md#signal_plies)
not already in `signal_evals`. Each position gets a `signal_nodes` search, and
the expected score is stored from our side. Commits once at the end.
