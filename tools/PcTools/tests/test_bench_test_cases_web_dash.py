#!/usr/bin/env python3
"""Fake-board tests for WEB-DASH-02..12 (kilnctrl.bench_test.cases_web_dash).

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_web_dash.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import kilnctrl.bench_test  # noqa: E402,F401  (wires every judge)
from kilnctrl.bench_test import cases_web_dash as D  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, Verdict  # noqa: E402


class FakeWeb:
    def __init__(self, pages=None):
        self.pages = pages or {}

    def goto(self, path):
        if path not in self.pages:
            raise RuntimeError(f"no page {path}")
        return self.pages[path]


def mk(gets=None, pages=None, posts=None, texts=None, **extra):
    gets = dict(gets or {})
    posted = []

    def get(path):
        v = gets.get(path)
        if isinstance(v, list):
            v = v.pop(0) if len(v) > 1 else v[0]
        return v if v is not None else (404, None)

    def post(path, fields):
        posted.append((path, dict(fields)))
        r = (posts or {}).get(path)
        return r(fields) if callable(r) else (r or (404, None))

    def post_raw(path, fields):
        posted.append((path, dict(fields)))
        return 200, "ok"

    ctx = {"http_get_json": get, "http_post_json": post, "web_client": FakeWeb(pages),
           "http_post_raw": post_raw,
           "http_get_text": lambda p: (texts or {}).get(p, (404, None)), "_sleep": lambda s: None}
    ctx.update(extra)
    ctx["_posted"] = posted
    return ctx


def run(cid, ctx):
    return REGISTRY[cid].judge(ctx)


class Registered(unittest.TestCase):
    def test_all_have_judges(self):
        for i in range(2, 13):
            self.assertIsNotNone(REGISTRY[f"WEB-DASH-{i:02d}"].judge)


class Dash02(unittest.TestCase):
    PAGE = {"/": "id=\"profileSelect\" /api/profiles/favorites appendGroup('Favorites'"}

    def test_pass(self):
        c = mk({"/api/profiles/favorites": (200, {"user_mask": 2, "builtin_mask": 1, "ids": [1, 128]}),
                "/api/profiles": (200, [{"id": 0}, {"id": 1}, {"id": 128}])}, self.PAGE)
        self.assertEqual(run("WEB-DASH-02", c).verdict, Verdict.PASS)

    def test_orphan_fails(self):
        c = mk({"/api/profiles/favorites": (200, {"user_mask": 0, "builtin_mask": 0, "ids": [5]}),
                "/api/profiles": (200, [{"id": 0}, {"id": 1}])}, self.PAGE)
        self.assertEqual(run("WEB-DASH-02", c).verdict, Verdict.FAIL)

    def test_no_favorites_inconclusive(self):
        c = mk({"/api/profiles/favorites": (200, {"user_mask": 0, "builtin_mask": 0, "ids": []}),
                "/api/profiles": (200, [{"id": 0}])}, self.PAGE)
        self.assertEqual(run("WEB-DASH-02", c).verdict, Verdict.INCONCLUSIVE)

    def test_missing_static_fails(self):
        c = mk({"/api/profiles/favorites": (200, {"user_mask": 2, "builtin_mask": 0, "ids": [1]}),
                "/api/profiles": (200, [{"id": 1}])}, {"/": "nothing"})
        self.assertEqual(run("WEB-DASH-02", c).verdict, Verdict.FAIL)


class Dash03(unittest.TestCase):
    PAGE = {"/": 'id="runBtn" id="profileSelect" /api/profile_exec/start'}

    def _ctx(self, samples, ok=True):
        c = mk(pages=self.PAGE)
        c["_hp01"] = {"ok": ok}
        c["_probe_results"] = {"hp01_tick": {"WEB-DASH-03": samples}}
        return c

    def test_pass(self):
        s = [{"state": "running", "profile_id": 7}, {"state": "done", "profile_id": 7}]
        self.assertEqual(run("WEB-DASH-03", self._ctx(s)).verdict, Verdict.PASS)

    def test_fail_no_running(self):
        s = [{"state": "idle", "profile_id": 0}]
        self.assertEqual(run("WEB-DASH-03", self._ctx(s)).verdict, Verdict.FAIL)

    def test_fail_last_not_done(self):
        s = [{"state": "running", "profile_id": 7}]
        self.assertEqual(run("WEB-DASH-03", self._ctx(s)).verdict, Verdict.FAIL)

    def test_not_run(self):
        self.assertEqual(run("WEB-DASH-03", mk()).verdict, Verdict.NOT_RUN)

    def test_all_errors_fail(self):
        r = run("WEB-DASH-03", self._ctx([{"error": "x"}]))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("ERROR:", r.reason)


class Dash04(unittest.TestCase):
    PAGE = {"/": 'id="feasPopupOverlay" feasPopupProceedBtn feasSummarize('}

    def test_pass(self):
        c = mk({"/api/profiles": (200, [{"id": 128}]),
                "/api/profile?id=128": (200, {"feasibility": "too_fast", "segments": [{"feasibility": "too_fast"}]})},
               self.PAGE)
        self.assertEqual(run("WEB-DASH-04", c).verdict, Verdict.PASS)

    def test_bogus_fails(self):
        c = mk({"/api/profiles": (200, [{"id": 128}]),
                "/api/profile?id=128": (200, {"feasibility": "bogus", "segments": []})}, self.PAGE)
        self.assertEqual(run("WEB-DASH-04", c).verdict, Verdict.FAIL)

    def test_all_ok_inconclusive(self):
        c = mk({"/api/profiles": (200, [{"id": 1}]),
                "/api/profile?id=1": (200, {"feasibility": "ok", "segments": []})}, self.PAGE)
        self.assertEqual(run("WEB-DASH-04", c).verdict, Verdict.INCONCLUSIVE)


class Dash05(unittest.TestCase):
    PAGE = {"/": 'id="pidPopupOverlay" pidPopupApplyBtn /api/zones/pid'}

    def _fake(self, restore_works=True):
        state = {"kp": 2.5, "ki": 0.01, "kd": 5.0, "n": 0}
        gets = {"/api/profile_exec": (200, {"state": "idle"}), "/api/autotune": (200, {"state": "idle"})}

        def post(fields):
            state["n"] += 1
            if restore_works or state["n"] == 1:
                state["kp"] = float(fields["kp"])
            return 200, {"ok": True}

        c = mk(gets, self.PAGE, {"/api/zones/pid": post}, suite="nightly")

        def get(p):
            if p == "/api/zones":
                return 200, {"zones": [{"pid_kp": state["kp"], "pid_ki": state["ki"], "pid_kd": state["kd"]}]}
            return gets.get(p, (404, None))

        c["http_get_json"] = get
        return c, state

    def test_pass_and_restored(self):
        c, st = self._fake()
        r = run("WEB-DASH-05", c)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertAlmostEqual(st["kp"], 2.5)

    def test_restore_not_roundtrip_fails(self):
        c, _ = self._fake(restore_works=False)
        r = run("WEB-DASH-05", c)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("RESTORE", r.reason)

    def test_read_only_suite_writes_nothing(self):
        c, _ = self._fake()
        c["suite"] = "smoke"
        self.assertEqual(run("WEB-DASH-05", c).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(c["_posted"], [])

    def _with_autotune(self, at):
        c, _ = self._fake()
        base = c["http_get_json"]
        c["http_get_json"] = lambda p: at if p == "/api/autotune" else base(p)
        return c

    def test_autotune_done_and_aborted_pass_gate(self):
        for s in ("done", "aborted"):
            c = self._with_autotune((200, {"state": s}))
            self.assertEqual(run("WEB-DASH-05", c).verdict, Verdict.PASS, s)

    def test_autotune_running_unknown_missing_refuse(self):
        for at in ((200, {"state": "stepping"}), (200, {"state": "relay_cycling"}), (200, {"state": "unknown"}),
                   (200, {}), (500, None)):
            c = self._with_autotune(at)
            self.assertEqual(run("WEB-DASH-05", c).verdict, Verdict.INCONCLUSIVE, at)
            self.assertEqual(c["_posted"], [], at)

    def test_busy_executor_writes_nothing(self):
        c, _ = self._fake()
        base = c["http_get_json"]
        c["http_get_json"] = lambda p: (200, {"state": "running"}) if p == "/api/profile_exec" else base(p)
        self.assertEqual(run("WEB-DASH-05", c).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(c["_posted"], [])


class Dash06(unittest.TestCase):
    PAGE = {"/app.js": "kc-stop-bar kc-pause-btn STOP FIRING ACKNOWLEDGE (firing complete) /api/profile_exec/pause"}

    def _ctx(self, states, paused="paused"):
        c = mk(pages=self.PAGE)
        c["_hp04"] = {"paused_state": paused}
        c["_probe_results"] = {"hp04_states": {"WEB-DASH-06": [{"state": s} for s in states]}}
        return c

    def test_pass(self):
        self.assertEqual(run("WEB-DASH-06", self._ctx(["running", "paused", "running", "done"])).verdict, Verdict.PASS)

    def test_fail_pause_not_seen(self):
        self.assertEqual(run("WEB-DASH-06", self._ctx(["running", "running", "running", "done"])).verdict, Verdict.FAIL)

    def test_inconclusive_no_done(self):
        self.assertEqual(run("WEB-DASH-06", self._ctx(["running", "paused", "running", "running"])).verdict,
                         Verdict.INCONCLUSIVE)

    def test_not_run(self):
        self.assertEqual(run("WEB-DASH-06", mk()).verdict, Verdict.NOT_RUN)


class Dash07(unittest.TestCase):
    PAGE = {"/": 'id="lastRunBanner" ackLastRunBtn /api/profile_exec/ack_last_run'}
    LR = {"present": True, "interrupted": False, "phase": "halted", "profile_id": 3}

    def test_pass_no_ack_for_foreign_record(self):
        c = mk({"/api/profile_exec": (200, {"state": "idle", "last_run": self.LR})}, self.PAGE, suite="nightly")
        self.assertEqual(run("WEB-DASH-07", c).verdict, Verdict.PASS)
        self.assertEqual(c["_posted"], [])

    def test_ack_bench_slot_record(self):
        lr = dict(self.LR, profile_id=7)
        has = (200, {"state": "idle", "last_run": lr})
        c = mk({"/api/profile_exec": [has, has, (200, {"state": "idle", "last_run": {"present": False}})],
                "/api/autotune": (200, {"state": "idle"})}, self.PAGE, suite="nightly")
        r = run("WEB-DASH-07", c)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(c["_posted"][0][0], "/api/profile_exec/ack_last_run")

    def test_ack_not_effective_fails(self):
        lr = dict(self.LR, profile_id=7)
        c = mk({"/api/profile_exec": (200, {"state": "idle", "last_run": lr}),
                "/api/autotune": (200, {"state": "idle"})}, self.PAGE, suite="nightly")
        self.assertEqual(run("WEB-DASH-07", c).verdict, Verdict.FAIL)

    def test_missing_last_run_fails(self):
        c = mk({"/api/profile_exec": (200, {"state": "idle"})}, self.PAGE)
        self.assertEqual(run("WEB-DASH-07", c).verdict, Verdict.FAIL)

    def test_absent_record_inconclusive(self):
        c = mk({"/api/profile_exec": (200, {"state": "idle", "last_run": {"present": False}})}, self.PAGE)
        self.assertEqual(run("WEB-DASH-07", c).verdict, Verdict.INCONCLUSIVE)

    def test_interrupted_contradiction_fails(self):
        lr = dict(self.LR, interrupted=True, phase="done")
        c = mk({"/api/profile_exec": (200, {"state": "idle", "last_run": lr})}, self.PAGE)
        self.assertEqual(run("WEB-DASH-07", c).verdict, Verdict.FAIL)


class Dash08(unittest.TestCase):
    PAGE = {"/": 'id="safetyTripBanner" clearTripBtn /api/safety/clear_trip'}
    TRIP = {"diag_ever_received": True, "diag_state": 4, "diag_age_ms": 200, "diag_trip_reason": 6}
    OK = {"diag_ever_received": True, "diag_state": 1, "diag_age_ms": 200, "diag_trip_reason": 0}

    def _ctx(self, tripped, cleared=None, outcome="s6a_latched"):
        c = mk(pages=self.PAGE)
        c["_otb01"] = {"outcome": outcome}
        c["_probe_results"] = {"otb01_tripped": {"WEB-DASH-08": {"bodies": [tripped]}},
                               "otb01_cleared": {"WEB-DASH-08": {"body": cleared or self.OK}}}
        return c

    def test_pass(self):
        self.assertEqual(run("WEB-DASH-08", self._ctx(self.TRIP)).verdict, Verdict.PASS)

    def test_fail_no_banner_during_trip(self):
        self.assertEqual(run("WEB-DASH-08", self._ctx(dict(self.TRIP, diag_state=1))).verdict, Verdict.FAIL)

    def test_fail_banner_after_clear(self):
        self.assertEqual(run("WEB-DASH-08", self._ctx(self.TRIP, cleared=self.TRIP)).verdict, Verdict.FAIL)

    def test_inconclusive_no_trip(self):
        self.assertEqual(run("WEB-DASH-08", self._ctx(self.TRIP, outcome="no_trip")).verdict, Verdict.INCONCLUSIVE)

    def test_not_run(self):
        self.assertEqual(run("WEB-DASH-08", mk()).verdict, Verdict.NOT_RUN)

    def test_probes_registered_on_both_windows(self):
        spec = REGISTRY["WEB-DASH-08"]
        self.assertEqual(spec.window_probe[0], "otb01_tripped")
        self.assertEqual(spec.extra_window_probes[0][0], "otb01_cleared")

    def test_tripped_probe_samples_status(self):
        c = mk({"/api/status": (200, self.TRIP)})
        self.assertEqual(D._status_probe(c)["bodies"], [self.TRIP])


class Dash09(unittest.TestCase):
    H = D._HISTORY_HEADER
    ROW1 = "0,30.00,25.00,0.500,0,25.00,0,0,25.00,0,0,1"
    ROW2 = "30,35.00,26.00,0.500,0,25.00,0,0,25.00,0,0,1"

    def _ctx(self, text, status=200):
        c = mk(texts={"/api/history.csv": (status, text)})
        c["_hp01"] = {"ok": True}
        return c

    def test_pass(self):
        r = run("WEB-DASH-09", self._ctx(f"{self.H}\n{self.ROW1}\n{self.ROW2}\n"))
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)

    def test_header_only_fails(self):
        self.assertEqual(run("WEB-DASH-09", self._ctx(self.H + "\n")).verdict, Verdict.FAIL)

    def test_no_zone0_fails(self):
        r1, r2 = self.ROW1[:-1] + "2", self.ROW2[:-1] + "2"
        self.assertEqual(run("WEB-DASH-09", self._ctx(f"{self.H}\n{r1}\n{r2}\n")).verdict, Verdict.FAIL)

    def test_decreasing_elapsed_fails(self):
        self.assertEqual(run("WEB-DASH-09", self._ctx(f"{self.H}\n{self.ROW2}\n{self.ROW1}\n")).verdict, Verdict.FAIL)

    def test_not_run(self):
        self.assertEqual(run("WEB-DASH-09", mk()).verdict, Verdict.NOT_RUN)


class Dash10to12(unittest.TestCase):
    def test_10(self):
        pages = {"/app.js": "kc-recovery-banner /api/status st.recovery_mode"}

        def mkc(b):
            return mk({"/api/status": (200, b)}, pages)

        self.assertEqual(run("WEB-DASH-10", mkc({"phase": "idle", "recovery_mode": False})).verdict, Verdict.PASS)
        self.assertEqual(run("WEB-DASH-10", mkc({"phase": "idle"})).verdict, Verdict.FAIL)
        self.assertEqual(run("WEB-DASH-10", mkc({"recovery_mode": True})).verdict, Verdict.INCONCLUSIVE)

    def test_11(self):
        pages = {"/app.js": "kc-setup-banner /api/readiness 'not_done' setSetupBanner(incomplete) href = '/setup'"}

        def mkc(s, pg=pages):
            return mk({"/api/readiness": (200, {"items": [{"key": "a", "status": s}]})}, pg)

        weak = {"/app.js": "kc-setup-banner /api/readiness 'not_done'"}
        self.assertEqual(run("WEB-DASH-11", mkc("not_done", weak)).verdict, Verdict.FAIL)
        self.assertEqual(run("WEB-DASH-11", mkc("ok", weak)).verdict, Verdict.PASS)

        self.assertEqual(run("WEB-DASH-11", mkc("not_done")).verdict, Verdict.PASS)
        self.assertEqual(run("WEB-DASH-11", mkc("pending")).verdict, Verdict.FAIL)

    def test_12(self):
        good = ("FAILURES_BEFORE_BANNER = 2 kc-conn-banner fetch('/api/profile_exec') "
                "consecutiveFailures >= FAILURES_BEFORE_BANNER")
        g = {"/api/profile_exec": (200, {"state": "idle"})}
        self.assertEqual(run("WEB-DASH-12", mk(g, {"/app.js": good})).verdict, Verdict.PASS)
        bad = good.replace("= 2", "= 3")
        self.assertEqual(run("WEB-DASH-12", mk(g, {"/app.js": bad})).verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
