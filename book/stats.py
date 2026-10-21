"""Posterior math for the learning book: shrinkage, Thompson sampling, tiers.

Pure functions with no database access, so they are easy to test. A move's
quality is a Beta posterior over its expected signal ``s``; layers shrink toward
the layer above (Stockfish prior -> all other opponents at this speed -> this
opponent). See [stats.py.md](stats.py.md).
"""
from __future__ import annotations

import random
from collections.abc import Hashable
from dataclasses import dataclass

NEUTRAL_PRIOR = 0.5
PICK_SAMPLES = 4000
MIN_BETA_PARAM = 1e-3  # keeps Beta valid when a prior has zero weight and no evidence


@dataclass
class MoveStats:
    """Weighted evidence for one move: ``n`` games, ``total`` summed signal."""

    n: float = 0.0
    total: float = 0.0

    def add(self, signal: float, weight: float = 1.0) -> None:
        """Count one game with signal ``signal`` at ``weight``."""
        self.n += weight
        self.total += weight * signal

    @property
    def mean(self) -> float | None:
        """Average signal, or None without evidence."""
        return self.total / self.n if self.n > 0.0 else None


@dataclass(frozen=True)
class Posterior:
    """Beta(alpha, beta) belief about a move's expected signal."""

    alpha: float
    beta: float

    @property
    def mean(self) -> float:
        """Posterior mean alpha / (alpha + beta)."""
        return self.alpha / (self.alpha + self.beta)

    def sample(self, rng: random.Random) -> float:
        """One Thompson draw from the posterior."""
        return rng.betavariate(self.alpha, self.beta)


def shrink(prior_mean: float, pseudo_games: float, evidence: MoveStats) -> Posterior:
    """Beta posterior: ``pseudo_games`` worth of ``prior_mean`` plus ``evidence``.

    Fractional signals update the Beta parameters fractionally: a game with
    signal s adds s to alpha and 1 - s to beta.
    """
    prior = min(max(prior_mean, 0.0), 1.0)
    return Posterior(alpha=max(pseudo_games * prior + evidence.total, MIN_BETA_PARAM),
                     beta=max(pseudo_games * (1.0 - prior) + evidence.n - evidence.total,
                              MIN_BETA_PARAM))


def layered_posterior(sf_expected: float | None, pooled: MoveStats, opponent: MoveStats,
                      k_stockfish: float, k_pool: float) -> tuple[Posterior, Posterior]:
    """The (pooled, opponent) posteriors for one move.

    The pooled layer shrinks the other opponents' results toward Stockfish's
    expected score; the opponent layer shrinks this opponent's results toward
    the pooled mean. With no games against this opponent the opponent layer
    equals the pooled mean, which is the "unknown opponent" fallback.
    """
    stockfish_prior = NEUTRAL_PRIOR if sf_expected is None else sf_expected
    pooled_post = shrink(stockfish_prior, k_stockfish, pooled)
    return pooled_post, shrink(pooled_post.mean, k_pool, opponent)


def thompson_pick(posteriors: dict[Hashable, Posterior], rng: random.Random) -> Hashable:
    """Draw once from each posterior and return the key with the largest draw."""
    return max(posteriors, key=lambda move: posteriors[move].sample(rng))


def pick_probabilities(posteriors: dict[Hashable, Posterior], rng: random.Random,
                       samples: int = PICK_SAMPLES) -> dict[Hashable, float]:
    """Monte-Carlo estimate of how often Thompson sampling picks each key."""
    if not posteriors:
        return {}
    wins = dict.fromkeys(posteriors, 0)
    for _ in range(samples):
        wins[thompson_pick(posteriors, rng)] += 1
    return {move: count / samples for move, count in wins.items()}


@dataclass(frozen=True)
class TierInput:
    """What the tier rule needs to know about one candidate."""

    tier: int
    opponent_n: float
    opponent_mean: float


def is_exhausted(candidate: TierInput, threshold: float, min_games: float) -> bool:
    """True when this opponent has seen the move enough and it keeps failing."""
    return candidate.opponent_n >= min_games and candidate.opponent_mean < threshold


def unlocked_tier(candidates: list[TierInput], threshold: float,
                  min_games: float) -> tuple[int, bool]:
    """Return (highest playable tier, every tier exhausted).

    Tier 0 is always playable. Tier t + 1 unlocks only when every candidate of
    tier <= t is exhausted against this opponent. The flag is true when even the
    highest existing tier is exhausted, which asks for a wider re-analysis.
    """
    tiers = sorted({c.tier for c in candidates})
    for tier in tiers:
        pool = [c for c in candidates if c.tier <= tier]
        if not all(is_exhausted(c, threshold, min_games) for c in pool):
            return tier, False
    return (tiers[-1] if tiers else 0), bool(tiers)
