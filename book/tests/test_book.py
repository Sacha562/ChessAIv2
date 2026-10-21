"""Unit tests for the opening book: stats, scoring, ingest, selector, server.

Run from the repo root: ``book/.venv/Scripts/python -m unittest discover -s book/tests -t .``
See [test_book.py.md](test_book.py.md).
"""
from __future__ import annotations

import random
import tempfile
import unittest
from pathlib import Path

import chess

from book import config, db, ingest, opponent_model, prep, scoring, selector, stats

ACCOUNT = "SmikkelBot"


def raw_game(game_id: str, moves: str, we_are_white: bool, winner: str | None,
             opponent: str = "RivalBot", speed: str = "blitz", created_at: int = 1) -> dict:
    """A minimal lichess export object."""
    me = {"user": {"name": ACCOUNT, "id": ACCOUNT.lower(), "title": "BOT"}, "rating": 2000}
    them = {"user": {"name": opponent, "id": opponent.lower(), "title": "BOT"}, "rating": 2100}
    players = {"white": me, "black": them} if we_are_white else {"white": them, "black": me}
    game = {"id": game_id, "variant": "standard", "speed": speed, "rated": True,
            "createdAt": created_at, "status": "resign", "players": players, "moves": moves}
    if winner:
        game["winner"] = winner
    return game


class DbTestCase(unittest.TestCase):
    """Gives each test a fresh database file and default settings."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.conn = db.connect(Path(self.tmp.name) / "book.sqlite")
        self.settings = config.load_settings(self.conn)

    def tearDown(self) -> None:
        self.conn.close()
        self.tmp.cleanup()

    def store(self, raw: dict) -> ingest.ParsedGame:
        game = ingest.parse_game(raw, ACCOUNT)
        assert game is not None
        ingest.store_game(self.conn, game, self.settings)
        self.conn.commit()
        return game

    def add_candidate(self, moves: tuple[str, ...], move: str, tier: int, expected: float) -> None:
        board = selector.board_from_moves(moves)
        self.conn.execute(
            "INSERT OR REPLACE INTO candidates(key, move, tier, source, sf_expected, enabled, added_at) "
            "VALUES(?, ?, ?, 'engine', ?, 1, 0)", (db.position_key(board), move, tier, expected))
        self.conn.execute("INSERT OR REPLACE INTO positions(key, fen, analysed_at) VALUES(?, ?, 1)",
                          (db.position_key(board), board.fen()))
        self.conn.commit()


class StatsTests(unittest.TestCase):
    def test_shrink_without_evidence_is_prior(self) -> None:
        post = stats.shrink(0.6, 4.0, stats.MoveStats())
        self.assertAlmostEqual(post.mean, 0.6)

    def test_unknown_opponent_falls_back_to_pooled(self) -> None:
        pooled = stats.MoveStats()
        for signal in (1.0, 1.0, 0.0):
            pooled.add(signal)
        pooled_post, opponent_post = stats.layered_posterior(0.5, pooled, stats.MoveStats(), 4.0, 5.0)
        self.assertAlmostEqual(opponent_post.mean, pooled_post.mean)

    def test_opponent_evidence_moves_posterior(self) -> None:
        losses = stats.MoveStats()
        for _ in range(10):
            losses.add(0.0)
        _, opponent_post = stats.layered_posterior(0.55, stats.MoveStats(), losses, 4.0, 5.0)
        self.assertLess(opponent_post.mean, 0.25)

    def test_zero_prior_weight_still_valid(self) -> None:
        post = stats.shrink(0.5, 0.0, stats.MoveStats())
        self.assertGreater(post.sample(random.Random(1)), -1.0)

    def test_thompson_prefers_clearly_better_move(self) -> None:
        posteriors = {"good": stats.Posterior(90.0, 10.0), "bad": stats.Posterior(10.0, 90.0)}
        picks = stats.pick_probabilities(posteriors, random.Random(7), samples=500)
        self.assertGreater(picks["good"], 0.99)

    def test_tier_unlocks_only_when_lower_tiers_exhausted(self) -> None:
        fresh = [stats.TierInput(0, 0.0, 0.5), stats.TierInput(1, 0.0, 0.5)]
        self.assertEqual(stats.unlocked_tier(fresh, 0.4, 3), (0, False))
        tier0_dead = [stats.TierInput(0, 5.0, 0.2), stats.TierInput(1, 0.0, 0.5)]
        self.assertEqual(stats.unlocked_tier(tier0_dead, 0.4, 3), (1, False))
        all_dead = [stats.TierInput(0, 5.0, 0.2), stats.TierInput(1, 4.0, 0.3)]
        self.assertEqual(stats.unlocked_tier(all_dead, 0.4, 3), (1, True))

    def test_few_games_never_exhaust(self) -> None:
        self.assertFalse(stats.is_exhausted(stats.TierInput(0, 2.0, 0.0), 0.4, 3))


class ScoringTests(unittest.TestCase):
    def test_window_past_game_end_uses_result(self) -> None:
        game = scoring.GameRow("g", 1.0, exit_ply=10, n_plies=12)
        self.assertAlmostEqual(scoring.eval_signal(game, {}, 20, 40, 2), 1.0)

    def test_missing_in_game_eval_is_pending(self) -> None:
        game = scoring.GameRow("g", 1.0, exit_ply=0, n_plies=100)
        self.assertIsNone(scoring.eval_signal(game, {0: 0.5}, 0, 4, 2))

    def test_window_mean(self) -> None:
        game = scoring.GameRow("g", 0.0, exit_ply=2, n_plies=100)
        evals = {2: 0.5, 4: 0.7, 6: 0.9}
        self.assertAlmostEqual(scoring.eval_signal(game, evals, 0, 4, 2), 0.7)

    def test_blend(self) -> None:
        self.assertAlmostEqual(scoring.blend(1.0, 0.5, 0.5), 0.75)
        self.assertAlmostEqual(scoring.blend(0.0, None, 0.5), 0.0)

    def test_signal_plies_start_at_exit(self) -> None:
        plies = scoring.signal_plies(10, 20, config.DEFAULT_SETTINGS)
        self.assertEqual(plies, [10, 12, 14, 16, 18])


class IngestTests(DbTestCase):
    def test_parse_result_from_our_side(self) -> None:
        game = ingest.parse_game(raw_game("a", "e4 e5", False, "black"), ACCOUNT)
        assert game is not None
        self.assertEqual(game.color, chess.BLACK)
        self.assertEqual(game.result, 1.0)
        self.assertEqual(game.opponent, "rivalbot")

    def test_skips_variants_and_aborts(self) -> None:
        variant = raw_game("v", "e4", True, None) | {"variant": "chess960"}
        aborted = raw_game("x", "e4", True, None) | {"status": "aborted"}
        self.assertIsNone(ingest.parse_game(variant, ACCOUNT))
        self.assertIsNone(ingest.parse_game(aborted, ACCOUNT))

    def test_store_queues_candidates_and_signal(self) -> None:
        self.store(raw_game("a", "e4 e5 Nf3 Nc6", True, "white"))
        kinds = {r["kind"] for r in self.conn.execute("SELECT kind FROM analysis_queue")}
        self.assertEqual(kinds, {"candidates", "signal"})
        ours = self.conn.execute("SELECT COUNT(*) FROM game_moves WHERE by_us = 1").fetchone()[0]
        self.assertEqual(ours, 2)

    def test_deep_one_off_positions_not_queued(self) -> None:
        self.settings["candidate_always_ply"] = 2
        self.store(raw_game("a", "e4 e5 Nf3 Nc6 Bb5", True, "black"))
        queued = self.conn.execute("SELECT COUNT(*) FROM analysis_queue "
                                   "WHERE kind = 'candidates'").fetchone()[0]
        self.assertEqual(queued, 1)
        self.store(raw_game("b", "e4 e5 Nf3 Nc6 Bc4", True, "black", created_at=2))
        self.assertEqual(ingest.rebuild_candidate_queue(self.conn, self.settings), 3)

    def test_won_game_queues_every_book_position(self) -> None:
        self.settings["candidate_always_ply"] = 2
        self.settings["max_book_ply"] = 4
        self.store(raw_game("a", "e4 e5 Nf3 Nc6 Bb5 a6", True, "white"))
        queued = self.conn.execute("SELECT COUNT(*) FROM analysis_queue "
                                   "WHERE kind = 'candidates'").fetchone()[0]
        self.assertEqual(queued, 2)

    def test_exit_inferred_without_log(self) -> None:
        self.store(raw_game("a", "e4 e5 Nf3 Nc6", True, "white"))
        row = self.conn.execute("SELECT exit_ply, exit_source FROM games").fetchone()
        self.assertEqual((row["exit_ply"], row["exit_source"]), (4, "inferred"))

    def test_exit_from_served_log(self) -> None:
        self.conn.execute("INSERT INTO book_events(game_id, ply, key, move, source, opponent, "
                          "speed, created_at) VALUES('a', 0, 0, 'e2e4', 'book', 'rivalbot', "
                          "'blitz', 0)")
        self.store(raw_game("a", "e4 e5 Nf3 Nc6", True, "white"))
        row = self.conn.execute("SELECT exit_ply, exit_source FROM games").fetchone()
        self.assertEqual((row["exit_ply"], row["exit_source"]), (2, "served"))


class SelectorTests(DbTestCase):
    def request(self, moves: tuple[str, ...], color: chess.Color = chess.WHITE,
                opponent: str = "rivalbot") -> selector.Request:
        return selector.Request("live", moves, color, opponent, "blitz")

    def test_book_pick_from_candidates(self) -> None:
        self.add_candidate((), "e2e4", 0, 0.55)
        decision = selector.choose(self.conn, self.request(()), self.settings, random.Random(1))
        self.assertEqual((decision.move, decision.source), ("e2e4", "book"))

    def test_locked_tier_not_played(self) -> None:
        self.add_candidate((), "e2e4", 0, 0.55)
        self.add_candidate((), "g2g4", 1, 0.45)
        for seed in range(20):
            decision = selector.choose(self.conn, self.request(()), self.settings, random.Random(seed))
            self.assertEqual(decision.move, "e2e4")

    def test_miss_queues_position(self) -> None:
        decision = selector.choose(self.conn, self.request(()), self.settings, random.Random(1))
        self.assertEqual(decision.source, "miss")
        job = self.conn.execute("SELECT kind, priority FROM analysis_queue").fetchone()
        self.assertEqual((job["kind"], job["priority"]), ("candidates", ingest.PRIORITY_MISS))

    def test_replay_won_game(self) -> None:
        self.add_candidate(("d2d4", "d7d5"), "c2c4", 0, 0.55)
        self.add_candidate(("d2d4", "d7d5"), "g1f3", 0, 0.56)
        self.store(raw_game("won", "d4 d5 c4 e6 Nc3", True, "white"))
        decision = selector.choose(self.conn, self.request(("d2d4", "d7d5")), self.settings,
                                   random.Random(1))
        self.assertEqual((decision.move, decision.source), ("c2c4", "replay"))
        self.assertAlmostEqual(decision.detail["loss"], 0.01)

    def test_replay_gate_skips_losing_move(self) -> None:
        self.add_candidate(("d2d4", "d7d5"), "c2c4", 0, 0.55)
        self.add_candidate(("d2d4", "d7d5"), "g2g4", 99, 0.45)
        self.store(raw_game("won", "d4 d5 g4", True, "white"))
        decision = selector.choose(self.conn, self.request(("d2d4", "d7d5")), self.settings,
                                   random.Random(1))
        self.assertEqual((decision.move, decision.source), ("c2c4", "book"))

    def test_replay_gate_falls_back_to_older_win(self) -> None:
        self.add_candidate(("d2d4", "d7d5"), "c2c4", 0, 0.55)
        self.add_candidate(("d2d4", "d7d5"), "g2g4", 99, 0.45)
        self.store(raw_game("old", "d4 d5 c4", True, "white", created_at=1))
        self.store(raw_game("new", "d4 d5 g4", True, "white", created_at=2))
        decision = selector.choose(self.conn, self.request(("d2d4", "d7d5")), self.settings,
                                   random.Random(1))
        self.assertEqual((decision.move, decision.source), ("c2c4", "replay"))
        self.assertEqual(decision.detail["game"], "old")

    def test_unevaluated_replay_skipped_and_queued(self) -> None:
        moves = ("d2d4", "d7d5")
        self.add_candidate(moves, "c2c4", 0, 0.55)
        self.store(raw_game("won", "d4 d5 Nf3", True, "white"))
        decision = selector.choose(self.conn, self.request(moves), self.settings, random.Random(1))
        self.assertEqual((decision.move, decision.source), ("c2c4", "book"))
        key = db.position_key(selector.board_from_moves(moves))
        job = self.conn.execute("SELECT status, priority FROM analysis_queue "
                                "WHERE kind = 'candidates' AND target = ?", (str(key),)).fetchone()
        self.assertEqual((job["status"], job["priority"]), ("queued", ingest.PRIORITY_MISS))

    def test_no_replay_of_lost_game_or_other_opponent(self) -> None:
        self.store(raw_game("lost", "d4 d5 c4", True, "black"))
        self.store(raw_game("other", "d4 d5 c4", True, "white", opponent="Someone"))
        decision = selector.choose(self.conn, self.request(("d2d4", "d7d5")), self.settings,
                                   random.Random(1))
        self.assertNotEqual(decision.source, "replay")

    def test_horizon(self) -> None:
        self.settings["max_book_ply"] = 24
        moves = ("g1f3", "g8f6", "f3g1", "f6g8") * 6
        decision = selector.choose(self.conn, self.request(moves), self.settings, random.Random(1))
        self.assertEqual(decision.source, "horizon")

    def test_not_our_turn(self) -> None:
        decision = selector.choose(self.conn, self.request((), chess.BLACK), self.settings,
                                   random.Random(1))
        self.assertIsNone(decision.move)

    def test_opponent_losses_unlock_next_tier(self) -> None:
        self.add_candidate((), "e2e4", 0, 0.55)
        self.add_candidate((), "d2d4", 1, 0.54)
        for i in range(5):
            self.store(raw_game(f"l{i}", "e4 e5", True, "black", created_at=i + 1))
        self.settings["replay_enabled"] = False
        board = chess.Board()
        analysis = selector.analyse_position(self.conn, board, "rivalbot", "blitz", self.settings)
        self.assertEqual(analysis.tier_limit, 1)
        pooled = selector.analyse_position(self.conn, board, "newcomer", "blitz", self.settings)
        self.assertEqual(pooled.tier_limit, 0)


class PrepTests(DbTestCase):
    def request(self, moves: tuple[str, ...], color: chess.Color = chess.WHITE,
                opponent: str = "rivalbot") -> selector.Request:
        return selector.Request("live", moves, color, opponent, "blitz")

    def test_prep_beats_replay_and_book(self) -> None:
        self.add_candidate((), "d2d4", 0, 0.55)
        self.store(raw_game("won", "d4 e5", True, "white"))
        prep.set_prep(self.conn, "rivalbot", (), "e2e4")
        decision = selector.choose(self.conn, self.request(()), self.settings, random.Random(1))
        self.assertEqual((decision.move, decision.source), ("e2e4", "prep"))

    def test_prep_is_opponent_specific(self) -> None:
        self.add_candidate((), "d2d4", 0, 0.55)
        prep.set_prep(self.conn, "rivalbot", (), "e2e4")
        decision = selector.choose(self.conn, self.request((), opponent="someone"), self.settings,
                                   random.Random(1))
        self.assertEqual(decision.move, "d2d4")

    def test_prep_ignores_horizon(self) -> None:
        moves = ("g1f3", "g8f6", "f3g1", "f6g8") * 10
        prep.set_prep(self.conn, "rivalbot", moves, "e2e4")
        decision = selector.choose(self.conn, self.request(moves), self.settings, random.Random(1))
        self.assertEqual(decision.source, "prep")

    def test_prep_line_sets_only_our_moves(self) -> None:
        line = ("e2e4", "d7d5", "e1e2", "d5e4")
        self.assertEqual(prep.set_prep_line(self.conn, "rivalbot", line, chess.BLACK), 2)
        entries = prep.list_prep(self.conn, "rivalbot")
        self.assertEqual([e["move"] for e in entries], ["d7d5", "d5e4"])
        self.assertEqual(entries[-1]["san"], "1.e4 d5 2.Ke2 dxe4")
        self.assertTrue(prep.remove_prep(self.conn, "rivalbot", ("e2e4",)))
        self.assertEqual(len(prep.list_prep(self.conn, "rivalbot")), 1)

    def test_illegal_prep_rejected(self) -> None:
        with self.assertRaises(ValueError):
            prep.set_prep(self.conn, "rivalbot", (), "e2e5")

    def test_prep_counts_as_served_for_exit(self) -> None:
        self.conn.execute("INSERT INTO book_events(game_id, ply, key, move, source, opponent, "
                          "speed, created_at) VALUES('a', 0, 0, 'e2e4', 'prep', 'rivalbot', "
                          "'blitz', 0)")
        self.store(raw_game("a", "e4 e5 Nf3 Nc6", True, "white"))
        row = self.conn.execute("SELECT exit_ply, exit_source FROM games").fetchone()
        self.assertEqual((row["exit_ply"], row["exit_source"]), (2, "served"))


class FakeStockfish:
    """Scores a position by a lookup on its FEN board part; 0.5 otherwise."""

    def __init__(self, scores: dict[str, float]) -> None:
        self.scores = scores
        self.calls = 0

    def position_expected(self, board: chess.Board, nodes: int, pov: chess.Color) -> float:
        self.calls += 1
        return self.scores.get(board.board_fen(), 0.5)


def bongcloud_history(case: "DbTestCase", games: int = 4) -> None:
    """Opponent 'bong' plays 1...e5 2...Ke7 as Black and 1.e4 2.Ke2 as White."""
    for i in range(games):
        second = ("Nf3", "Nc3", "Bc4", "d3")[i % 4]
        case.store(raw_game(f"w{i}", f"e4 e5 {second} Ke7", True, "white", "bong", created_at=i + 1))
        reply = ("e5", "c5", "e6", "d5")[i % 4]
        case.store(raw_game(f"b{i}", f"e4 {reply} Ke2", False, "white", "bong", created_at=i + 1))


class OpponentModelTests(DbTestCase):
    def test_ply_patterns_found(self) -> None:
        bongcloud_history(self)
        patterns = opponent_model.ply_patterns(self.conn, "bong", self.settings)
        self.assertEqual(patterns[("white", 1)], "e7e5")
        self.assertEqual(patterns[("white", 3)], "e8e7")
        self.assertEqual(patterns[("black", 0)], "e2e4")
        self.assertEqual(patterns[("black", 2)], "e1e2")

    def test_inconsistent_ply_is_not_a_pattern(self) -> None:
        bongcloud_history(self)
        patterns = opponent_model.ply_patterns(self.conn, "bong", self.settings)
        self.assertNotIn(("white", 2), patterns)  # that ply is ours, never theirs
        self.assertNotIn(("black", 1), patterns)

    def test_history_beats_pattern(self) -> None:
        bongcloud_history(self)
        self.store(raw_game("x", "e4 e5 f4 Nc6", True, "white", "bong", created_at=99))
        board = selector.board_from_moves(("e2e4", "e7e5", "f2f4"))
        patterns = opponent_model.ply_patterns(self.conn, "bong", self.settings)
        replies = opponent_model.predict_replies(self.conn, "bong", board, chess.WHITE, patterns)
        self.assertEqual((replies[0].move, replies[0].source), ("b8c6", "history"))
        fresh = selector.board_from_moves(("e2e4", "e7e5", "d2d4"))
        replies = opponent_model.predict_replies(self.conn, "bong", fresh, chess.WHITE, patterns)
        self.assertEqual((replies[0].move, replies[0].source), ("e8e7", "pattern"))

    def test_deepen_scores_moves_against_predicted_reply(self) -> None:
        bongcloud_history(self)
        after_f4 = selector.board_from_moves(("e2e4", "e7e5", "f2f4", "e8e7")).board_fen()
        after_nf3 = selector.board_from_moves(("e2e4", "e7e5", "g1f3", "e8e7")).board_fen()
        stockfish = FakeStockfish({after_f4: 0.95, after_nf3: 0.80})

        def ensure(board: chess.Board) -> None:
            moves = tuple(m.uci() for m in board.move_stack)
            if moves == ("e2e4", "e7e5"):
                self.add_candidate(moves, "g1f3", 0, 0.55)
                self.add_candidate(moves, "f2f4", 2, 0.45)
            elif not moves:
                self.add_candidate(moves, "e2e4", 0, 0.55)

        self.settings["max_book_ply"] = 4
        opponent_model.deepen(self.conn, stockfish, "bong", self.settings, ensure)
        board = selector.board_from_moves(("e2e4", "e7e5"))
        values = opponent_model.model_values(self.conn, "bong", db.position_key(board))
        self.assertAlmostEqual(values["f2f4"]["expected"], 0.95)
        self.assertEqual(values["f2f4"]["reply"], "e8e7")
        calls = stockfish.calls
        opponent_model.deepen(self.conn, stockfish, "bong", self.settings, ensure)
        self.assertEqual(stockfish.calls, calls)  # unchanged predictions are cached

    def test_model_reweights_and_promotes_for_that_opponent_only(self) -> None:
        bongcloud_history(self)
        moves = ("e2e4", "e7e5")
        self.add_candidate(moves, "g1f3", 0, 0.55)
        self.add_candidate(moves, "f2f4", 2, 0.45)
        key = db.position_key(selector.board_from_moves(moves))
        self.conn.execute("INSERT INTO opponent_model VALUES('bong', ?, 'f2f4', 0.95, 'e8e7', 1.0, "
                          "'pattern', 'e8e7:1.000', 0)", (key,))
        board = selector.board_from_moves(moves)
        vs_bong = {v.move: v for v in selector.analyse_position(self.conn, board, "bong", "blitz",
                                                                self.settings).candidates}
        self.assertTrue(vs_bong["f2f4"].playable)
        self.assertGreater(vs_bong["f2f4"].opponent_post.mean, vs_bong["g1f3"].opponent_post.mean)
        vs_other = {v.move: v for v in selector.analyse_position(self.conn, board, "other", "blitz",
                                                                 self.settings).candidates}
        self.assertFalse(vs_other["f2f4"].playable)

    def test_predictable_opponent_auto_queues_deepen(self) -> None:
        bongcloud_history(self, games=5)
        jobs = self.conn.execute("SELECT target FROM analysis_queue WHERE kind = 'deepen'").fetchall()
        self.assertEqual([j["target"] for j in jobs], ["bong"])


class ServerTests(DbTestCase):
    def test_add_manual_candidate_and_read_position(self) -> None:
        try:
            from fastapi.testclient import TestClient
        except ImportError:  # httpx missing
            self.skipTest("fastapi.testclient needs httpx")
        from book.server import create_app
        static = config.StaticConfig(Path(self.tmp.name) / "book.sqlite", Path("stockfish"),
                                     "127.0.0.1", 0)
        with TestClient(create_app(static, start_worker=False)) as client:
            added = client.post("/api/candidates", json={"moves": [], "move": "Nf3", "tier": 0})
            self.assertEqual(added.json()["move"], "g1f3")
            pos = client.get("/api/position", params={"moves": "", "opponent": "", "speed": "blitz"})
            moves = {c["move"]: c for c in pos.json()["candidates"]}
            self.assertEqual(moves["g1f3"]["source"], "manual")
            bad = client.post("/api/candidates", json={"moves": [], "move": "Ke2"})
            self.assertEqual(bad.status_code, 400)
            pick = client.post("/api/shim/select", json={"game_id": "x", "moves": [], "color": "white",
                                                         "opponent": "Anyone", "speed": "blitz"})
            self.assertEqual(pick.json()["move"], "g1f3")
            line = client.post("/api/prep/line", json={"opponent": "Bongaclang", "color": "white",
                                                       "line": ["e4", "e5", "f4", "Ke7"]})
            self.assertEqual(line.json()["prepared"], 2)
            listed = client.get("/api/prep", params={"opponent": "bongaclang"}).json()
            self.assertEqual([p["move"] for p in listed], ["e2e4", "f2f4"])
            served = client.post("/api/shim/select", json={"game_id": "y", "moves": ["e2e4", "e7e5"],
                                                           "color": "white", "opponent": "Bongaclang",
                                                           "speed": "bullet"})
            self.assertEqual((served.json()["move"], served.json()["source"]), ("f2f4", "prep"))


if __name__ == "__main__":
    unittest.main()
