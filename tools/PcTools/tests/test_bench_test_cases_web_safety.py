#!/usr/bin/env python3
"""Fake-board tests for kilnctrl.bench_test.cases_web_safety (WEB-SAF/COMM/RDY/
WIZ/SET/DISP). Nothing touches urllib or hardware. Static-token checks run
against the real firmware page sources.

Run: python -m pytest tools/PcTools/tests/test_bench_test_cases_web_safety.py -q
"""
from __future__ import annotations

import copy
import json
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_web_safety as W  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, CaseResult, Verdict  # noqa: E402

HTTP_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW", "App", "drivers", "http")
PAGES = {
    "/safety": "safety_page.html", "/settings/commissioning": "safety_commissioning_page.html",
    "/commissioning_shared.js": "commissioning_shared.js", "/readiness": "readiness_page.html",
    "/setup": "setup_wizard_page.html", "/settings": "settings_page.html",
    "/settings/display": "settings_display_page.html",
}
_real: dict = {}


def real_page(path):
    if path not in _real:
        with open(os.path.join(HTTP_DIR, PAGES[path]), encoding="utf-8") as fh:
            _real[path] = fh.read()
    return _real[path]


class Board:
    """Dict-backed fake. gets: path -> body (dict) or callable; posts recorded."""

    def __init__(self, gets=None, pages=None, posts=None):
        self.gets = {"/api/profile_exec": {"state": "idle"}, "/api/autotune": {"state": "idle"}}
        self.gets.update(gets or {})
        self.pages = {p: real_page(p) for p in PAGES}
        self.pages.update(pages or {})
        self.posts = []
        self.post_handlers = posts or {}

    def ctx(self, **extra):
        def get_json(path):
            b = self.gets.get(path)
            if b is None:
                return 404, None
            return 200, copy.deepcopy(b() if callable(b) else b)

        def post_json(path, fields):
            self.posts.append((path, dict(fields)))
            h = self.post_handlers.get(path)
            return h(fields) if h else (200, {"ok": True})

        def get_text(path):
            t = self.pages.get(path)
            return (200, t) if t is not None else (404, None)

        def post_raw(path, fields):
            self.posts.append((path, dict(fields)))
            h = self.post_handlers.get(path)
            return h(fields) if h else (200, "ok")

        c = {"http_get_json": get_json, "http_post_json": post_json, "http_get_text": get_text,
             "http_post_raw": post_raw, "suite": "web", "_sleep": lambda s: None}
        c.update(extra)
        return c


def run(cid, ctx) -> CaseResult:
    return REGISTRY[cid].judge(ctx)


def V(res):
    return res.verdict


def status(**kw):
    s = {"diag_ever_received": True, "diag_trip_mask": 0, "diag_warn_mask": 0, "diag_state": 1,
         "diag_trip_reason": 0, "trip_event_ever_received": True}
    s.update(kw)
    return s


SAF_GOOD = {"/api/status": status()}
DIAG_OK = {"ok": True, "diag_ever_received": True, "diag_boot_reason": 1, "diag_context_frames_ok": 5,
           "diag_context_frames_bad": 0, "diag_tx_frames_dropped": 0, "diag_log_frames_dropped": 0,
           "safety_tc_reconfig_gave_up": 0, "safety_s1_abs_max_disabled": 0, "safety_s8_rate_guard_disabled": 0}


def param(pid, value=1, set_=True):
    p = {"id": pid, "set": set_}
    if set_:
        p["value"] = value
    return p


COMM_GOOD = {"/api/safety/commissioning": {
    "link_up": True, "stale": False, "unset_reporting_reliable": True, "commissioned": True,
    "live_config_crc": 7, "cached_config_crc": 7, "relay_type": "contactor",
    "params": [param(i) for i in (260, 529, 257, 782, 793, 259, 258, 265, 799)],
    "ct_cal": [{"has_value": True}] * 3}}
RDY_GOOD = {"/api/readiness": {"items": [{"key": k, "status": "ok"} for k in
                                        ("safety_trip", "recovery_mode", "crash_report", "estop_verified", "network")]}}
ZONES_GOOD = {"/api/zones": {"thermo_count": 3, "zones": [
    {"zone_type": 0, "tc_type": 1, "cal_offset_c": 0.0} for _ in range(3)]}}


class WiringTest(unittest.TestCase):
    def test_every_id_has_judge(self):
        self.assertEqual(len(W._CASE_FUNCS), 28)
        for cid in W._CASE_FUNCS:
            self.assertIs(REGISTRY[cid].judge, W._CASE_FUNCS[cid])

    def test_observer_registry(self):
        self.assertEqual(REGISTRY["WEB-SAF-03"].depends_on, "OT-B01")
        self.assertEqual(REGISTRY["WEB-COMM-04"].depends_on, "HP-01")
        self.assertEqual(REGISTRY["WEB-WIZ-07"].depends_on, "WEB-COMM-03")
        self.assertEqual(REGISTRY["WEB-WIZ-09"].depends_on, "WEB-ZONE-05")
        for cid in ("WEB-SAF-03", "WEB-RDY-04", "WEB-WIZ-11", "WEB-COMM-04"):
            self.assertIsNotNone(REGISTRY[cid].window_probe)

    def test_empty_board_never_passes_never_writes(self):
        b = Board()
        b.gets = {}
        b.pages = {}
        for cid, fn in W._CASE_FUNCS.items():
            res = fn(b.ctx())
            self.assertNotEqual(res.verdict, Verdict.PASS, cid)
            self.assertTrue(res.reason, cid)
        self.assertEqual(b.posts, [])


class RealPageTokens(unittest.TestCase):
    def test_static_cases_pass_with_good_inputs(self):
        b2 = Board(gets=COMM_GOOD)
        self.assertEqual(V(run("WEB-COMM-05", b2.ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-COMM-06", b2.ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-COMM-02", b2.ctx())), Verdict.PASS)
        pr = {"hp01_running": {"WEB-COMM-04": {"state": "running"}}}
        self.assertEqual(V(run("WEB-COMM-04", b2.ctx(_hp01={"x": 1}, _probe_results=pr))), Verdict.PASS)
        self.assertEqual(V(run("WEB-RDY-03", Board(gets=RDY_GOOD).ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-WIZ-06", Board(gets=ZONES_GOOD).ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-SET-03", Board().ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-DISP-04", Board().ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-WIZ-08", Board().ctx())), Verdict.INCONCLUSIVE)
        self.assertEqual(b2.posts, [])

    def test_missing_token_fails(self):
        for cid, path, tok, gets in (
            ("WEB-COMM-05", "/settings/commissioning", 'id="benchBtn"', COMM_GOOD),
            ("WEB-COMM-04", "/commissioning_shared.js", "checkBusy", COMM_GOOD),
            ("WEB-RDY-03", "/readiness", "scrollIntoView", RDY_GOOD),
            ("WEB-SET-03", "/settings", 'data-scope="all"', {}),
            ("WEB-DISP-04", "/settings/display", 'id="themeBtn"', {}),
            ("WEB-WIZ-06", "/setup", "step4ZoneTypeConfirmLines", ZONES_GOOD),
        ):
            b = Board(gets=gets)
            b.pages[path] = real_page(path).replace(tok, "XX_REMOVED_XX")
            pr = {"hp01_running": {"WEB-COMM-04": {"state": "running"}}}
            res = run(cid, b.ctx(_hp01={"x": 1}, _probe_results=pr))
            self.assertEqual(V(res), Verdict.FAIL, cid)

    def test_comm05_visible_benchbtn_fails(self):
        b = Board()
        html = re.sub(r'(<[^>]*id="benchBtn"[^>]*?)style="[^"]*"', r"\1", real_page("/settings/commissioning"))
        b.pages["/settings/commissioning"] = html
        self.assertEqual(V(run("WEB-COMM-05", b.ctx())), Verdict.FAIL)

    def test_disp04_fetch_in_theme_script_fails(self):
        b = Board()
        html = real_page("/settings/display").replace(
            "localStorage.setItem('kilnctl-theme'", "fetch('/api/x');localStorage.setItem('kilnctl-theme'", 1)
        b.pages["/settings/display"] = html
        self.assertEqual(V(run("WEB-DISP-04", b.ctx())), Verdict.FAIL)


class SafetyTest(unittest.TestCase):
    def diag(self, **kw):
        d = {"ever_received": True, "trip_mask": 0, "warn_mask": 0, "state": 1, "trip_reason": 0}
        d.update(kw)
        return lambda: d

    def test_saf02(self):
        b = Board(gets=SAF_GOOD)
        self.assertEqual(V(run("WEB-SAF-02", b.ctx(safety_get_diag=self.diag()))), Verdict.PASS)
        self.assertEqual(V(run("WEB-SAF-02", b.ctx(safety_get_diag=self.diag(trip_mask=0x20)))), Verdict.FAIL)
        b2 = Board(gets={"/api/status": status(diag_ever_received=False)})
        res = run("WEB-SAF-02", b2.ctx(safety_get_diag=self.diag(ever_received=False)))
        self.assertEqual(V(res), Verdict.INCONCLUSIVE)

    def test_saf03(self):
        otb = {"outcome": "s6a_latched", "clear_ok": True}
        win = {"WEB-SAF-03": {"trip_event_ever_received": True, "diag_trip_mask": 0x20}}

        def ctx(o=otb, w=win):
            return Board().ctx(_otb01=o, _probe_results={"otb01_tripped": w})

        self.assertEqual(V(run("WEB-SAF-03", ctx())), Verdict.PASS)
        bad = {"WEB-SAF-03": {"trip_event_ever_received": True, "diag_trip_mask": 0x40}}
        self.assertEqual(V(run("WEB-SAF-03", ctx(w=bad))), Verdict.FAIL)
        self.assertEqual(V(run("WEB-SAF-03", ctx(o={"outcome": "s6a_latched", "clear_ok": False}))), Verdict.FAIL)
        self.assertEqual(V(run("WEB-SAF-03", ctx(o={"outcome": "other_or_no_link"}))), Verdict.NOT_RUN)
        self.assertEqual(V(run("WEB-SAF-03", Board().ctx(_otb01=otb))), Verdict.NOT_RUN)
        self.assertEqual(V(run("WEB-SAF-03", ctx(w={"WEB-SAF-03": {"error": "x"}}))), Verdict.INCONCLUSIVE)
        b = Board()
        b.pages["/safety"] = re.sub(r'(id="clearTripBtn"[^>]*?)\sdisabled', r"\1", real_page("/safety"))
        self.assertEqual(V(run("WEB-SAF-03", b.ctx(_otb01=otb))), Verdict.FAIL)

    def test_saf03_probe_reads_only(self):
        b = Board(gets=SAF_GOOD)
        self.assertEqual(W._saf03_probe(b.ctx())["diag_trip_mask"], 0)
        self.assertEqual(b.posts, [])

    def test_saf04(self):
        self.assertEqual(V(run("WEB-SAF-04", Board(gets={"/api/status?diag=1": DIAG_OK}).ctx())), Verdict.PASS)
        d = dict(DIAG_OK)
        del d["diag_boot_reason"]
        self.assertEqual(V(run("WEB-SAF-04", Board(gets={"/api/status?diag=1": d}).ctx())), Verdict.FAIL)
        d = dict(DIAG_OK, diag_ever_received=False)
        self.assertEqual(V(run("WEB-SAF-04", Board(gets={"/api/status?diag=1": d}).ctx())), Verdict.INCONCLUSIVE)


class CommissioningTest(unittest.TestCase):
    def test_comm02(self):
        self.assertEqual(V(run("WEB-COMM-02", Board(gets=COMM_GOOD).ctx())), Verdict.PASS)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["link_up"] = False
        self.assertEqual(V(run("WEB-COMM-02", Board(gets=g).ctx())), Verdict.INCONCLUSIVE)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["params"] = [param(260)]
        self.assertEqual(V(run("WEB-COMM-02", Board(gets=g).ctx())), Verdict.FAIL)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["params"][0] = param(260, set_=False)
        self.assertEqual(V(run("WEB-COMM-02", Board(gets=g).ctx())), Verdict.INCONCLUSIVE)

    def test_comm03_readonly(self):
        b = Board(gets=COMM_GOOD)
        self.assertEqual(V(run("WEB-COMM-03", b.ctx())), Verdict.PASS)
        self.assertEqual(b.posts, [], "COMM-03 must never POST")
        seq = iter([7, 8])
        base = COMM_GOOD["/api/safety/commissioning"]
        g = {"/api/safety/commissioning": lambda: dict(base, live_config_crc=(c := next(seq)), cached_config_crc=c)}
        self.assertEqual(V(run("WEB-COMM-03", Board(gets=g).ctx())), Verdict.FAIL)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["cached_config_crc"] = 9
        self.assertEqual(V(run("WEB-COMM-03", Board(gets=g).ctx())), Verdict.FAIL)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["stale"] = True
        self.assertEqual(V(run("WEB-COMM-03", Board(gets=g).ctx())), Verdict.INCONCLUSIVE)

    def test_comm04(self):
        def mk(w):
            return Board(gets=COMM_GOOD).ctx(_hp01={"x": 1}, _probe_results=w)

        self.assertEqual(V(run("WEB-COMM-04", mk({"hp01_running": {"WEB-COMM-04": {"state": "idle"}}}))), Verdict.FAIL)
        self.assertEqual(V(run("WEB-COMM-04", mk({}))), Verdict.INCONCLUSIVE)
        self.assertEqual(V(run("WEB-COMM-04", Board().ctx())), Verdict.NOT_RUN)
        probe = W._comm04_probe(Board(gets={"/api/profile_exec": {"state": "running"}}).ctx())
        self.assertEqual(probe, {"state": "running"})

    def test_comm05_06_never_post(self):
        b = Board(gets=COMM_GOOD)
        run("WEB-COMM-05", b.ctx())
        run("WEB-COMM-06", b.ctx())
        self.assertEqual(b.posts, [])

    def test_comm06(self):
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["ct_cal"][2] = {"has_value": False}
        self.assertEqual(V(run("WEB-COMM-06", Board(gets=g).ctx())), Verdict.FAIL)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["params"] = [param(265, 0)]
        self.assertEqual(V(run("WEB-COMM-06", Board(gets=g).ctx())), Verdict.INCONCLUSIVE)
        g = copy.deepcopy(COMM_GOOD)
        g["/api/safety/commissioning"]["ct_cal"] = []
        self.assertEqual(V(run("WEB-COMM-06", Board(gets=g).ctx())), Verdict.FAIL)

    def test_comm07(self):
        state = {"relay_type": "contactor"}
        g = {"/api/safety/commissioning":
             lambda: dict(COMM_GOOD["/api/safety/commissioning"], relay_type=state["relay_type"])}
        ok = {"/api/safety/commissioning/relay_type": lambda f: (200, {"ok": True, "persisted": True})}
        b = Board(gets=g, posts=ok)
        self.assertEqual(V(run("WEB-COMM-07", b.ctx())), Verdict.PASS)
        self.assertTrue(b.posts and all(f == {"type": "contactor"} for _, f in b.posts))
        b = Board(gets=g, posts={"/api/safety/commissioning/relay_type": lambda f: (200, {"ok": True, "persisted": False})})
        self.assertEqual(V(run("WEB-COMM-07", b.ctx())), Verdict.FAIL)

    def test_comm07_gates(self):
        g = {"/api/safety/commissioning": COMM_GOOD["/api/safety/commissioning"]}
        b = Board(gets=dict(g, **{"/api/profile_exec": {"state": "running"}}))
        self.assertEqual(V(run("WEB-COMM-07", b.ctx())), Verdict.INCONCLUSIVE)
        self.assertEqual(b.posts, [])
        b = Board(gets=g)
        self.assertEqual(V(run("WEB-COMM-07", b.ctx(suite="smoke"))), Verdict.INCONCLUSIVE)
        self.assertEqual(b.posts, [])


class ReadinessTest(unittest.TestCase):
    def test_rdy02(self):
        self.assertEqual(V(run("WEB-RDY-02", Board(gets=RDY_GOOD).ctx())), Verdict.PASS)
        g = copy.deepcopy(RDY_GOOD)
        g["/api/readiness"]["items"][3]["status"] = "not_done"
        self.assertEqual(V(run("WEB-RDY-02", Board(gets=g).ctx())), Verdict.FAIL)
        g = copy.deepcopy(RDY_GOOD)
        g["/api/readiness"]["items"][0]["status"] = "cannot_yet"
        self.assertEqual(V(run("WEB-RDY-02", Board(gets=g).ctx())), Verdict.INCONCLUSIVE)
        g = copy.deepcopy(RDY_GOOD)
        g["/api/readiness"]["items"][4]["status"] = "weird"
        self.assertEqual(V(run("WEB-RDY-02", Board(gets=g).ctx())), Verdict.FAIL)

    def test_rdy03_dup_key(self):
        g = copy.deepcopy(RDY_GOOD)
        g["/api/readiness"]["items"].append({"key": "network", "status": "ok"})
        self.assertEqual(V(run("WEB-RDY-03", Board(gets=g).ctx())), Verdict.FAIL)

    def test_rdy04(self):
        otb = {"outcome": "s6a_latched"}

        def win(**k):
            return {"otb01_tripped": {"WEB-RDY-04": dict(
                {"trip_status": "not_done", "start_status": 409, "readiness_item": "safety_trip"}, **k)}}

        def mk(w):
            return Board().ctx(_otb01=otb, _probe_results=w)

        self.assertEqual(V(run("WEB-RDY-04", mk(win()))), Verdict.PASS)
        self.assertEqual(V(run("WEB-RDY-04", mk(win(start_status=200)))), Verdict.FAIL)
        self.assertEqual(V(run("WEB-RDY-04", mk(win(trip_status="ok")))), Verdict.FAIL)
        self.assertEqual(V(run("WEB-RDY-04", mk(win(trip_status="cannot_yet")))), Verdict.INCONCLUSIVE)
        self.assertEqual(V(run("WEB-RDY-04", mk({}))), Verdict.NOT_RUN)
        self.assertEqual(V(run("WEB-RDY-04", Board().ctx())), Verdict.NOT_RUN)

    def test_rdy04_probe_posts_only_probe_without_id(self):
        gets = {"/api/readiness": {"items": [{"key": "safety_trip", "status": "not_done"}]}}
        b = Board(gets=gets, posts={
            "/api/profile_exec/start": lambda f: (409, json.dumps({"readiness_item": "safety_trip"}))})
        out = W._rdy04_probe(b.ctx())
        self.assertEqual(out["start_status"], 409)
        self.assertEqual(out["readiness_item"], "safety_trip")
        self.assertEqual(b.posts, [("/api/profile_exec/start", {"probe": "1"})])


def progress(**over):
    steps = {str(i): {"state": "pending", "note": "", "ts": 1} for i in range(12)}
    steps.update(over)
    return {"version": 1, "steps": steps}


class WizardTest(unittest.TestCase):
    def test_wiz02(self):
        b = Board(gets=dict(RDY_GOOD, **{"/api/setup/progress": progress()}))
        res = run("WEB-WIZ-02", b.ctx())
        self.assertEqual(V(res), Verdict.PASS)
        self.assertEqual(res.observed["resume_step"], 0)
        p = progress()
        p["steps"]["3"]["state"] = "bogus"
        self.assertEqual(V(run("WEB-WIZ-02", Board(gets={"/api/setup/progress": p}).ctx())), Verdict.FAIL)
        p = progress()
        del p["steps"]["7"]
        self.assertEqual(V(run("WEB-WIZ-02", Board(gets={"/api/setup/progress": p}).ctx())), Verdict.FAIL)

    def test_wiz03_round_trip(self):
        st = {"time_tz": "EST5EDT", "temp_unit": "C"}

        def tz(f):
            st["time_tz"] = f["tz"]
            return 200, {"ok": True}

        def unit(f):
            st["temp_unit"] = f["unit"]
            return 200, {"ok": True}

        b = Board(gets={"/api/status": lambda: dict(st)}, posts={"/api/settings/tz": tz, "/api/unit_pref": unit})
        self.assertEqual(V(run("WEB-WIZ-03", b.ctx())), Verdict.PASS)
        self.assertEqual(st, {"time_tz": "EST5EDT", "temp_unit": "C"})

    def test_wiz03_restore_not_sticking_fails(self):
        st = {"time_tz": "EST5EDT", "temp_unit": "C"}

        def unit_stuck(f):
            if f["unit"] == "F":
                st["temp_unit"] = "F"
            return 200, {"ok": True}

        def tz(f):
            st["time_tz"] = f["tz"]
            return 200, {"ok": True}

        b = Board(gets={"/api/status": lambda: dict(st)}, posts={"/api/settings/tz": tz, "/api/unit_pref": unit_stuck})
        res = run("WEB-WIZ-03", b.ctx())
        self.assertEqual(V(res), Verdict.FAIL)
        self.assertIn("restore", res.reason)

    def test_wiz03_unreadable_status(self):
        b = Board()
        self.assertEqual(V(run("WEB-WIZ-03", b.ctx())), Verdict.INCONCLUSIVE)
        self.assertEqual(b.posts, [])

    def test_wiz04_05(self):
        z = {"thermo_count": 3, "zones": [{"tc_type": 1, "cal_offset_c": 0.0, "zone_type": 0}] * 3}
        s = {"zones": [{"actual_valid": True, "actual_c": 25.0}] * 3}
        b = Board(gets={"/api/zones": z, "/api/status": s})
        self.assertEqual(V(run("WEB-WIZ-04", b.ctx())), Verdict.PASS)
        self.assertEqual(V(run("WEB-WIZ-05", b.ctx())), Verdict.PASS)
        s2 = {"zones": [{"actual_valid": True, "actual_c": 25.0}, {"actual_valid": False, "actual_c": 0},
                        {"actual_valid": True, "actual_c": 1}]}
        self.assertEqual(V(run("WEB-WIZ-04", Board(gets={"/api/zones": z, "/api/status": s2}).ctx())), Verdict.FAIL)
        z2 = dict(z, thermo_count=2)
        self.assertEqual(V(run("WEB-WIZ-04", Board(gets={"/api/zones": z2, "/api/status": s}).ctx())),
                         Verdict.INCONCLUSIVE)
        z3 = dict(z, zones=[{"tc_type": 9, "cal_offset_c": 0.0}] * 3)
        self.assertEqual(V(run("WEB-WIZ-05", Board(gets={"/api/zones": z3}).ctx())), Verdict.FAIL)

    def test_wiz07_09_alias(self):
        for cid, host in (("WEB-WIZ-07", "WEB-COMM-03"), ("WEB-WIZ-09", "WEB-ZONE-05")):
            b = Board()
            self.assertEqual(V(run(cid, b.ctx(_results={host: CaseResult(Verdict.PASS)}))), Verdict.PASS)
            self.assertEqual(V(run(cid, b.ctx(_results={host: CaseResult(Verdict.FAIL, reason="x")}))), Verdict.FAIL)
            self.assertEqual(V(run(cid, b.ctx(_results={host: CaseResult(Verdict.INCONCLUSIVE, reason="x")}))),
                             Verdict.INCONCLUSIVE)
            self.assertEqual(V(run(cid, b.ctx(_results={}))), Verdict.NOT_RUN)
            self.assertEqual(b.posts, [])

    def test_wiz10(self):
        store = progress()

        def post(f):
            store["steps"][f["step"]] = {"state": f["state"], "note": f["note"], "ts": 2}
            return 200, {"ok": True}

        b = Board(gets={"/api/setup/progress": lambda: copy.deepcopy(store)}, posts={"/api/setup/progress": post})
        self.assertEqual(V(run("WEB-WIZ-10", b.ctx())), Verdict.PASS)
        self.assertEqual(store["steps"]["10"]["state"], "pending")
        self.assertEqual(store["steps"]["11"]["state"], "pending")

    def test_wiz10_restore_lie_fails(self):
        store = progress()

        def post(f):
            if f["state"] == "pending":
                return 200, {"ok": True}  # restore silently ignored
            store["steps"][f["step"]] = {"state": f["state"], "note": f["note"], "ts": 2}
            return 200, {"ok": True}

        b = Board(gets={"/api/setup/progress": lambda: copy.deepcopy(store)}, posts={"/api/setup/progress": post})
        self.assertEqual(V(run("WEB-WIZ-10", b.ctx())), Verdict.FAIL)

    def test_wiz10_503_and_gate(self):
        b = Board(gets={"/api/setup/progress": progress()}, posts={"/api/setup/progress": lambda f: (503, None)})
        self.assertEqual(V(run("WEB-WIZ-10", b.ctx())), Verdict.INCONCLUSIVE)
        b = Board(gets={"/api/setup/progress": progress(), "/api/profile_exec": {"state": "running"}})
        self.assertEqual(V(run("WEB-WIZ-10", b.ctx())), Verdict.INCONCLUSIVE)
        self.assertEqual(b.posts, [])

    def test_wiz11(self):
        before = {"steps": {"10": {"state": "done", "note": ""}}}

        def mk(p, otb=None):
            return Board(gets={"/api/setup/progress": p}).ctx(
                _otb01=otb or {"esp_restart_confirmed": True, "outcome": "no_trip"},
                _probe_results={"otb01_before_reset": {"WEB-WIZ-11": before}})

        same = {"steps": {"10": {"state": "done", "note": "", "ts": 99}}}
        self.assertEqual(V(run("WEB-WIZ-11", mk(same))), Verdict.PASS)
        diff = {"steps": {"10": {"state": "pending", "note": ""}}}
        self.assertEqual(V(run("WEB-WIZ-11", mk(diff))), Verdict.FAIL)
        unconfirmed = {"esp_restart_confirmed": False, "outcome": "inconclusive_x"}
        self.assertEqual(V(run("WEB-WIZ-11", mk(same, unconfirmed))), Verdict.INCONCLUSIVE)
        self.assertEqual(V(run("WEB-WIZ-11", Board().ctx())), Verdict.NOT_RUN)


class SettingsTest(unittest.TestCase):
    def test_set02(self):
        g = {"/api/cfgfs/format_pending": {"pending": False}, "/api/status": {}}
        self.assertEqual(V(run("WEB-SET-02", Board(gets=g).ctx())), Verdict.PASS)
        g2 = {"/api/cfgfs/format_pending": {"pending": True, "reason": "r"}, "/api/status": {}}
        self.assertEqual(V(run("WEB-SET-02", Board(gets=g2).ctx())), Verdict.INCONCLUSIVE)
        g3 = {"/api/cfgfs/format_pending": {"pending": "no"}}
        self.assertEqual(V(run("WEB-SET-02", Board(gets=g3).ctx())), Verdict.FAIL)

    def test_set03_no_post_and_order(self):
        b = Board()
        run("WEB-SET-03", b.ctx())
        self.assertEqual(b.posts, [])
        html = real_page("/settings")
        i = html.find("querySelectorAll('.danger-btn')")
        broken = html[:i] + html[i:].replace("kcConfirm(", "kcConfirmX(", 1)
        b.pages["/settings"] = broken
        self.assertEqual(V(run("WEB-SET-03", b.ctx())), Verdict.FAIL)

    def test_set04(self):
        def mk(otb):
            return Board().ctx(_otb01=otb)

        self.assertEqual(V(run("WEB-SET-04", mk({"outcome": "no_trip", "esp_restart_confirmed": True}))), Verdict.PASS)
        ok = {"outcome": "s6a_latched", "esp_restart_confirmed": True, "clear_ok": True}
        self.assertEqual(V(run("WEB-SET-04", mk(ok))), Verdict.PASS)
        other = {"outcome": "other_or_no_link", "esp_restart_confirmed": True}
        self.assertEqual(V(run("WEB-SET-04", mk(other))), Verdict.FAIL)
        inc = {"outcome": "inconclusive_x", "esp_restart_confirmed": False}
        self.assertEqual(V(run("WEB-SET-04", mk(inc))), Verdict.INCONCLUSIVE)
        self.assertEqual(V(run("WEB-SET-04", Board().ctx())), Verdict.NOT_RUN)


DP = {"brightness_percent": 80, "timeout_setting": 5, "keep_on_while_firing": True, "display_on_error": False,
      "brightness_inert": False}


class DisplayTest(unittest.TestCase):
    def mk(self, lie=False):
        st = dict(DP)

        def post(f):
            if lie and int(f["brightness"]) == DP["brightness_percent"]:
                return 200, {"ok": True}  # restore silently ignored
            st["brightness_percent"] = int(f["brightness"])
            st["timeout_setting"] = int(f["timeout"])
            st["keep_on_while_firing"] = f["keep_on_while_firing"] == "1"
            st["display_on_error"] = f["display_on_error"] == "1"
            return 200, {"ok": True}

        return st, Board(gets={"/api/settings/display_power": lambda: dict(st)},
                         posts={"/api/settings/display_power": post})

    def test_disp02_pass_restores(self):
        st, b = self.mk()
        self.assertEqual(V(run("WEB-DISP-02", b.ctx())), Verdict.PASS)
        self.assertEqual(st, DP)

    def test_disp02_restore_fail(self):
        st, b = self.mk(lie=True)
        self.assertEqual(V(run("WEB-DISP-02", b.ctx())), Verdict.FAIL)

    def test_disp02_gate_and_unreadable(self):
        st, b = self.mk()
        b.gets["/api/autotune"] = {"state": "running"}
        self.assertEqual(V(run("WEB-DISP-02", b.ctx())), Verdict.INCONCLUSIVE)
        self.assertEqual(b.posts, [])
        self.assertEqual(V(run("WEB-DISP-02", Board().ctx())), Verdict.INCONCLUSIVE)

    def test_disp03(self):
        b = Board(gets={"/api/settings/display_power": DP})
        self.assertEqual(V(run("WEB-DISP-03", b.ctx())), Verdict.INCONCLUSIVE)
        ctx = b.ctx(board_fw_commit="abcdef1", worktree_head="abcdef1234")
        self.assertEqual(V(run("WEB-DISP-03", ctx)), Verdict.PASS)
        b = Board(gets={"/api/settings/display_power": dict(DP, brightness_inert=True)})
        ctx = b.ctx(board_fw_commit="abcdef1", worktree_head="abcdef1234")
        self.assertEqual(V(run("WEB-DISP-03", ctx)), Verdict.FAIL)
        b = Board(gets={"/api/settings/display_power": dict(DP, brightness_inert="x")})
        self.assertEqual(V(run("WEB-DISP-03", b.ctx())), Verdict.FAIL)


class AuditTwoSafetyTests(unittest.TestCase):
    def test_l4_ignored_brightness_field_fails_even_at_50(self):
        st = dict(DP, brightness_percent=50)
        b = Board(gets={"/api/settings/display_power": lambda: dict(st)},
                  posts={"/api/settings/display_power": lambda f: (200, {"ok": True})})  # ignores every field
        self.assertEqual(V(run("WEB-DISP-02", b.ctx())), Verdict.FAIL)

    def test_l3_empty_zones_array_does_not_pass_wiz06(self):
        gets = dict(ZONES_GOOD)
        gets["/api/zones"] = {"thermo_count": 3, "zones": []}
        b = Board(gets=dict(gets, **RDY_GOOD, **{"/api/setup/progress": progress()}))
        res = run("WEB-WIZ-06", b.ctx())
        self.assertNotEqual(V(res), Verdict.PASS, res.reason)


if __name__ == "__main__":
    unittest.main()
