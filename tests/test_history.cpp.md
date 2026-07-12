# Test: `test_history.cpp`

Unit tests for the [quiet-move ordering heuristics](../src/history.hpp.md)
(`src/history.*`). Strategy: drive a `History` through its public update API
([`updateQuietCutoff`](../src/history.hpp.md#historyupdatequietcutoff)) and assert the
observable contract — killer LIFO, history bonus/malus signs, the saturating gravity
bound, countermove round-trips, and **continuation-history** reward/malus + context
keying — then end-to-end checks that these signals actually move a quiet's rank in
[`orderMoves`](../src/movepick.hpp.md#ordermoves).

A local `cutoff()` helper records a quiet fail-high at ply 0 for the side to move
(taking an optional [`ContHistContext`](../src/history.hpp.md#struct-conthistcontext)),
`rankOf()` finds a move's position in an ordered list, `pieceIndex()` returns a move's
colour+type index, and `ctx1()` builds a one-ply continuation context from raw indices.

## Test Code

- **Test Code:** [test_history.cpp](test_history.cpp)
- **Module under test:** [history.hpp.md](../src/history.hpp.md)
- **Runner / entry point:** [test_main.cpp.md](test_main.cpp.md)

## How to Build & Run

Built and run by the doctest binary (see [build.md](../build.md)):

```bash
make test            # builds chessai-tests and runs every case
# or, via CMake:  ctest --test-dir build --output-on-failure
```

Also run as part of the commit gate ([run_checks.py.md](../scripts/run_checks.py.md)).

## Tests

### `a fresh History is empty`

A default-constructed `History` reports zero butterfly score, `NO_MOVE` for both killer
slots, and `NO_MOVE` for any countermove.

### `killers keep the two most recent distinct cutoffs, newest first`

After one cutoff the move is in killer slot 0; a second distinct cutoff shifts it to
slot 1 and the new move into slot 0; repeating the same cutoff neither shuffles nor
duplicates.

### `history rewards the cutoff move and penalises the quiets tried first`

After a cutoff by `cut` with `fail` searched (and failed) first, `quietScore(cut) > 0`
and `quietScore(fail) < 0` — the bonus/malus signs.

### `history saturates within +/-MAX_HISTORY under repeated updates`

1000 max-depth updates in the same direction leave the rewarded move's score positive
and the penalised move's score negative, both strictly within
[`±MAX_HISTORY`](../src/history.hpp.md#max_history) — the gravity update never
overflows the band.

### `countermove stores and retrieves the refutation keyed by the previous move`

On the position after `e2e4`, recording `e7e5` as the cutoff stores it as the
countermove to `e2e4`; querying with `e2e4` returns `e7e5`, and querying with `NO_MOVE`
returns `NO_MOVE`.

### `a killer move floats above the ordinary quiets`

With a ply-0 killer set and no captures available (start position), `orderMoves` places
that killer at index 0.

### `a history-penalised quiet sinks below a neutral quiet`

A quiet given negative history (searched-then-failed before a cutoff) is ordered after
an untouched, zero-history quiet.

### `continuation history rewards the cutoff and penalises the quiets tried first`

Under a fixed one-ply context, a cutoff by `cut` with `fail` searched (and failed) first
leaves `continuationScore(cut) > 0` and `continuationScore(fail) < 0` — the same
bonus/malus signs as butterfly history, but keyed on the recent-move context.

### `continuation history is keyed by the recent-move context`

A cutoff recorded under one context is invisible from a *different* context (same move,
different prior move → score `0`) and from an empty context (no prior move → `0`).

### `continuation history is silent when disabled`

With `UseContHist` off ([`setEnabled`](../src/history.hpp.md#historysetenabled)),
`continuationScore` returns `0` even after a cutoff that would otherwise reward it.

### `a continuation-history quiet floats above a neutral quiet`

With the other quiet signals disabled so only continuation history can move a rank, a
quiet rewarded under the node's context is ordered ahead of an untouched quiet by
[`orderMoves`](../src/movepick.hpp.md#ordermoves).
