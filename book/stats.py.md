# Module: `book.stats`

Pure posterior math for the learning book. There is no database access, so
everything here is unit-testable in isolation.

A candidate move's quality against an opponent is a **Beta posterior over its
expected signal `s`** ([scoring.py.md](scoring.py.md)). Evidence is
fractional: a game with `s = 0.7` adds 0.7 to α and 0.3 to β. Two layers of
shrinkage implement the fallback hierarchy:

```
Stockfish expected score ──k_stockfish──> pooled layer   (other opponents, same speed)
pooled posterior mean    ──k_pool──────>  opponent layer (this opponent; other speeds × weight)
```

With no games against an opponent, the opponent layer's mean equals the pooled
mean, which is exactly the "unknown opponent → overall weights for this time
control" behaviour.

## Source File

- **Code:** [stats.py](stats.py)

## Related Test Documentation

- [tests/test_book.py.md](tests/test_book.py.md)

## Objects / Interfaces

| Constant | Value | Meaning |
|----------|-------|---------|
| `NEUTRAL_PRIOR` | `0.5` | Prior mean when a candidate has no Stockfish score yet (a fresh manual move). |
| `PICK_SAMPLES` | `4000` | Monte-Carlo draws for [`pick_probabilities`](#pick_probabilities). |
| `MIN_BETA_PARAM` | `1e-3` | Floor for α and β so a zero-weight prior with no evidence stays a valid Beta. |

### `class MoveStats`

Mutable dataclass of weighted evidence: `n` (weighted games) and `total`
(weighted sum of signals). `add(signal, weight)` accumulates; `mean` is
`total / n` or `None`.

### `class Posterior`

Frozen dataclass `Beta(alpha, beta)`. `mean` is `α / (α + β)`; `sample(rng)` is
one Thompson draw (`random.betavariate`).

### `class TierInput`

Frozen dataclass: `tier`, `opponent_n` (weighted games against this opponent),
`opponent_mean` (opponent-layer posterior mean).

## Functions

### `shrink`

`Beta(k·m + total, k·(1−m) + n − total)`, where `m` is the prior mean (clamped
to [0, 1]) and `k` the pseudo-game count; both parameters are floored at
`MIN_BETA_PARAM`.

### `layered_posterior`

**Returns:** `(pooled_post, opponent_post)` for one move: the pooled layer
shrinks `pooled` evidence toward `sf_expected` (or 0.5) with `k_stockfish`; the
opponent layer shrinks `opponent` evidence toward the pooled mean with `k_pool`.

### `thompson_pick`

One Beta draw per key; returns the key with the largest draw. This balances
exploiting moves that score well with trying under-sampled ones, in proportion
to how plausible it is that they are best.

### `pick_probabilities`

Monte-Carlo estimate (`samples` rounds of [`thompson_pick`](#thompson_pick)) of
how often each key is chosen. Shown as **Pick** on the dashboard.

**Returns:** `dict[key, float]` summing to 1 (empty dict for no posteriors).

### `is_exhausted`

True when `opponent_n ≥ min_games` and `opponent_mean < threshold`: the
opponent has seen the move enough and it keeps failing.

### `unlocked_tier`

The per-opponent tier rule. Walks the existing tiers in ascending order; the
first tier whose pool (all candidates with tier ≤ it) still contains a
non-exhausted candidate is the playable limit.

**Returns:** `(tier_limit, all_exhausted)`. `all_exhausted` is true only when
even the highest existing tier's pool is exhausted; the selector then queues a
widen analysis ([selector.py.md](selector.py.md#_book_pick)). With no
candidates it returns `(0, False)`.
