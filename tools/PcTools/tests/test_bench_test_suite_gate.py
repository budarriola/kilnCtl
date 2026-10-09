"""Fail-closed write gate: with ctx["suite"] absent (or read-only), no WEB
judge may issue a write, and WEB-COMM-07 reports ERROR on restore failure."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import board_lock  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
import kilnctrl.bench_test.cases_web  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_dash  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_diag  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_misc  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_prof  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_rw  # noqa: E402,F401
import kilnctrl.bench_test.cases_web_safety  # noqa: E402,F401

# Judges whose body writes to the board (POSTs, import, restore round trips).
WRITING_IDS = [
    "WEB-COMM-07", "WEB-WIZ-03", "WEB-WIZ-10", "WEB-DISP-02",
    "WEB-DASH-13", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-SEC-03", "WEB-SEC-04",
    "WEB-DIAG-09", "WEB-KCFG-02", "WEB-LOG-02", "WEB-PROF-02", "WEB-PROF-08",
    "WEB-ZONE-02", "WEB-ZONE-05",
]


class _Boom:
    def __getattr__(self, name):
        def f(*a, **k):
            raise AssertionError(f"client.{name} used without a mutating suite")
        return f


def _ctx(posts, **extra):
    def get(path):
        if path in ("/api/profile_exec", "/api/autotune"):
            return 200, {"state": "idle"}
        return 200, {}

    def post(path, fields):
        posts.append(path)
        return 200, {"ok": True}

    def post_raw(path, fields):
        posts.append(path)
        return 200, "ok"

    def post_body(path, body):
        posts.append(path)
        return 200, "ok"

    c = {"http_get_json": get, "http_post_json": post, "http_post_raw": post_raw,
         "http_post_json_body": post_body, "http_get_text": lambda p: (200, ""),
         "web_client": _Boom(), "sec_client": _Boom(), "host": "127.0.0.1:1",
         "web_username": "u", "web_password": "p", "_sleep": lambda s: None}
    c.update(extra)
    return c


class AbsentSuiteNeverWrites(unittest.TestCase):
    def test_helper(self):
        self.assertIsNotNone(board_lock.write_refusal({}))
        self.assertIsNotNone(board_lock.write_refusal({"suite": "smoke"}))
        self.assertIsNone(board_lock.write_refusal({"suite": "web"}))

    def _check(self, **extra):
        for cid in WRITING_IDS:
            posts = []
            try:
                res = R.get_case(cid).judge(_ctx(posts, **extra))
            except KeyError:
                self.fail(f"{cid} not registered")
            with self.subTest(case=cid, suite=extra.get("suite", "<absent>")):
                self.assertEqual(posts, [], f"{cid} wrote with no mutating suite")
                self.assertNotEqual(res.verdict, Verdict.PASS)

    def test_absent_suite(self):
        self._check()

    def test_read_only_suite(self):
        self._check(suite="smoke")


class DiagAutotuneMissingState(unittest.TestCase):
    def test_missing_autotune_state_refuses(self):
        from kilnctrl.bench_test import cases_web_diag as D
        c = _ctx([], suite="web")
        orig = c["http_get_json"]
        c["http_get_json"] = lambda p: (200, {}) if p == "/api/autotune" else orig(p)
        self.assertIn("autotune", D.mutating_gate(c))


class Comm07RestoreError(unittest.TestCase):
    def test_restore_failure_is_error(self):
        sys.path.insert(0, os.path.dirname(__file__))
        import test_bench_test_cases_web_safety as T
        state = {"n": 0, "relay": "contactor"}

        def handler(fields):
            state["n"] += 1
            if state["n"] == 1:  # the write "succeeds" but leaves the board changed
                state["relay"] = "mercury"
                return 200, {"ok": True, "persisted": True}
            return 500, {"ok": False}  # the restore fails

        g = {"/api/safety/commissioning": lambda: dict(
            T.COMM_GOOD["/api/safety/commissioning"], relay_type=state["relay"])}
        b = T.Board(gets=g, posts={"/api/safety/commissioning/relay_type": handler})
        res = T.run("WEB-COMM-07", b.ctx())
        self.assertEqual(res.verdict, Verdict.FAIL)
        self.assertIn("ERROR:", res.reason)
        self.assertEqual(state["n"], 2)


if __name__ == "__main__":
    unittest.main()
