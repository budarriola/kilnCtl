"""Fake-board tests for cases_web_diag (WEB-DIAG-02..11, WEB-OTA-02..08)."""
import os
import tempfile
import unittest

from kilnctrl.bench_test import cases_web_diag as D  # noqa: F401 - wires judges
from kilnctrl.bench_test import registry as R
from kilnctrl.bench_test.registry import CaseResult, Verdict

PASS, FAIL, INC, SKIP, NR = (Verdict.PASS, Verdict.FAIL, Verdict.INCONCLUSIVE, Verdict.SKIP, Verdict.NOT_RUN)


class _Web:
    def __init__(self, pages):
        self.pages = pages

    def goto(self, path):
        if path not in self.pages:
            raise RuntimeError("404 " + path)
        return self.pages[path]


def ctx_for(routes=None, pages=None, posts=None, **extra):
    routes = routes or {}
    posted = []

    def get(path):
        v = routes.get(path)
        if v is None:
            return None, None
        return v if isinstance(v, tuple) else (200, v)

    def post(path, fields):
        posted.append((path, fields))
        return (posts or {}).get(path, (200, {"ok": True}))

    c = {"suite": "web", "http_get_json": get, "http_post_json": post, "web_client": _Web(pages or {}), "_posted": posted}
    c.update(extra)
    return c


def run(cid, ctx):
    return R.get_case(cid).judge(ctx)


IDLE = {"/api/profile_exec": {"state": "idle"}, "/api/autotune": {"state": "idle"}}

DIAG_HTML = ('<div id="crashCard"></div><button id="crashAckBtn"></button><button id="crashClearBtn"></button>'
             '<div id="staleBanner"></div>staleAge function setStale(){} failCount '
             '<input id="dangerAccept"><button id="dangerEnterBtn" disabled></button>'
             '<div id="dangerLive"></div><button id="dangerExitBtn"></button> relayWearCard relay-reset-btn')


class Diag02(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(run("WEB-DIAG-02", ctx_for({"/api/crash_report": {"present": False}},
                                                    {"/diagnostics": DIAG_HTML})).verdict, PASS)

    def test_fail_missing_marker(self):
        r = run("WEB-DIAG-02", ctx_for({"/api/crash_report": {"present": False}},
                                       {"/diagnostics": DIAG_HTML.replace("crashAckBtn", "x")}))
        self.assertEqual(r.verdict, FAIL)

    def test_inconclusive_present(self):
        self.assertEqual(run("WEB-DIAG-02", ctx_for({"/api/crash_report": {"present": True}})).verdict, INC)
        self.assertEqual(run("WEB-DIAG-02", ctx_for({"/api/crash_report": {"present": True, "acknowledged": False}})).verdict, INC)

    def test_acknowledged_crash_still_checks_page(self):
        ack = {"present": True, "acknowledged": True}
        self.assertEqual(run("WEB-DIAG-02", ctx_for({"/api/crash_report": ack}, {"/diagnostics": DIAG_HTML})).verdict, PASS)
        r = run("WEB-DIAG-02", ctx_for({"/api/crash_report": ack}, {"/diagnostics": DIAG_HTML.replace("crashAckBtn", "x")}))
        self.assertEqual(r.verdict, FAIL)


CSV = "# Name,Type,SubType,Offset,Size\nnvs,data,nvs,0x9000,0x6000\napp,app,ota_0,0x10000,0x100000\n"
PARTS = {"running": "app", "partitions": [
    {"label": "nvs", "type": 1, "subtype": 2, "offset": 0x9000, "size": 0x6000, "encrypted": False},
    {"label": "app", "type": 0, "subtype": 0x10, "offset": 0x10000, "size": 0x100000, "encrypted": False}]}


class Diag03(unittest.TestCase):
    def ctx(self, parts):
        f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
        f.write(CSV)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return ctx_for({"/api/partitions": parts}, partitions_csv_path=f.name)

    def test_pass(self):
        self.assertEqual(run("WEB-DIAG-03", self.ctx(PARTS)).verdict, PASS)

    def test_fail_offset(self):
        bad = {**PARTS, "partitions": [dict(PARTS["partitions"][0], offset=0x8000), PARTS["partitions"][1]]}
        self.assertEqual(run("WEB-DIAG-03", self.ctx(bad)).verdict, FAIL)

    def test_inconclusive_no_csv(self):
        c = ctx_for({"/api/partitions": PARTS}, partitions_csv_path="/nonexistent.csv")
        self.assertEqual(run("WEB-DIAG-03", c).verdict, INC)


CFG = {"mounted": True, "status": "ok", "file_count": 7,
       "capacity": {"known": True, "total_bytes": 100, "used_bytes": 10, "free_bytes": 90},
       "dual_write": {"items": [{"name": "zones"}]}}


class Diag04(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(run("WEB-DIAG-04", ctx_for({"/api/cfgfs": CFG}, _fl07_cfgfs=CFG)).verdict, PASS)

    def test_fail_vs_fl07(self):
        other = dict(CFG, file_count=3)
        self.assertEqual(run("WEB-DIAG-04", ctx_for({"/api/cfgfs": CFG}, _fl07_cfgfs=other)).verdict, FAIL)

    def test_inconclusive_unmounted(self):
        self.assertEqual(run("WEB-DIAG-04", ctx_for({"/api/cfgfs": {"mounted": False}})).verdict, INC)


def _tc(state="ok", fs=0):
    return {"channel_count": 3, "channels": [{"channel": i, "state": state, "fault_status": fs, "stale": False}
                                              for i in range(3)], "safety": {"state": "ok"}}


class Diag05(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(run("WEB-DIAG-05", ctx_for({"/api/thermo/faults": _tc()})).verdict, PASS)

    def test_fail(self):
        self.assertEqual(run("WEB-DIAG-05", ctx_for({"/api/thermo/faults": _tc("faulted", 0)})).verdict, FAIL)

    def test_inconclusive_no_link(self):
        d = _tc()
        d["safety"] = {"state": "no_link"}
        self.assertEqual(run("WEB-DIAG-05", ctx_for({"/api/thermo/faults": d})).verdict, INC)


def _status(n=5, tier="none"):
    return {"io_ready": True, "relay_life": [{"relay": i, "cycles": 1, "tier": tier, "type": "ssr",
                                               "rated": None, "percent": None} for i in range(n)]}


class Diag06(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(run("WEB-DIAG-06", ctx_for({"/api/status": _status()}, {"/diagnostics": DIAG_HTML})).verdict, PASS)

    def test_fail_four_entries(self):
        self.assertEqual(run("WEB-DIAG-06", ctx_for({"/api/status": _status(4)}, {"/diagnostics": DIAG_HTML})).verdict, FAIL)

    def test_inconclusive_io_not_ready(self):
        self.assertEqual(run("WEB-DIAG-06", ctx_for({"/api/status": {"io_ready": False}})).verdict, INC)


class Diag09(unittest.TestCase):
    routes = dict(IDLE, **{"/api/diagnostics/danger": {"active": False}})

    def test_pass_409(self):
        c = ctx_for(self.routes, {"/diagnostics": DIAG_HTML}, posts={"/api/diagnostics/danger/relay": (409, {})})
        self.assertEqual(run("WEB-DIAG-09", c).verdict, PASS)

    def test_fail_200(self):
        c = ctx_for(self.routes, {"/diagnostics": DIAG_HTML},
                    posts={"/api/diagnostics/danger/relay": (200, {"ok": True})})
        self.assertEqual(run("WEB-DIAG-09", c).verdict, FAIL)

    def test_inconclusive_active(self):
        c = ctx_for({"/api/diagnostics/danger": {"active": True}}, {"/diagnostics": DIAG_HTML})
        self.assertEqual(run("WEB-DIAG-09", c).verdict, INC)

    def test_no_probe_when_busy(self):
        r = dict(self.routes, **{"/api/profile_exec": {"state": "running"}})
        c = ctx_for(r, {"/diagnostics": DIAG_HTML})
        self.assertEqual(run("WEB-DIAG-09", c).verdict, SKIP)
        self.assertEqual(c["_posted"], [])


def _timing():
    o = {"count": 2, "last": 5, "min": 1, "max": 9, "mean": 5}
    return {"display_flush_us": dict(o), "thermo_read_us": dict(o), "link_reply_us": dict(o, timeouts=0)}


class Diag10(unittest.TestCase):
    base = {"/api/board_temps": {"esp32_c": 40.0, "thermo_cj_c": [20, None, 21]}}

    def test_pass_501(self):
        r = dict(self.base, **{"/api/diagnostics/timing": _timing(), "/api/debug/lwip_stats": (501, None)})
        self.assertEqual(run("WEB-DIAG-10", ctx_for(r)).verdict, PASS)

    def test_fail_mean_out_of_range(self):
        t = _timing()
        t["thermo_read_us"]["mean"] = 99
        r = dict(self.base, **{"/api/diagnostics/timing": t, "/api/debug/lwip_stats": (501, None)})
        self.assertEqual(run("WEB-DIAG-10", ctx_for(r)).verdict, FAIL)


class Diag11(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(run("WEB-DIAG-11", ctx_for(pages={"/diagnostics": DIAG_HTML})).verdict, PASS)

    def test_fail(self):
        self.assertEqual(run("WEB-DIAG-11", ctx_for(pages={"/diagnostics": "x"})).verdict, FAIL)

    def test_inconclusive(self):
        self.assertEqual(run("WEB-DIAG-11", ctx_for()).verdict, INC)


class Ota02(unittest.TestCase):
    def go(self, res):
        return run("WEB-OTA-02", {"_probe_results": {"hp01_running": {"WEB-OTA-02": res}}})

    def test_pass(self):
        self.assertEqual(self.go({"status": 200, "body": {"ok": False, "reason": "firing"}}).verdict, PASS)

    def test_fail_allows(self):
        self.assertEqual(self.go({"status": 200, "body": {"ok": True}}).verdict, FAIL)

    def test_inconclusive_error(self):
        self.assertEqual(self.go({"error": "x"}).verdict, INC)

    def test_not_run_without_window(self):
        self.assertEqual(run("WEB-OTA-02", {}).verdict, NR)


OTA_HTML = ('id="espPicker" id="espFile" id="espUpdateBtn" function pushImage id="espRollbackBtn" '
            '/api/ota/esp/rollback id="picoInfo" id="picoPicker" id="picoFile" id="picoUpdateBtn" '
            'id="picoRollbackBtn" id="recoveryExitBox" style="display:none" id="recoveryExitBtn"')


OTA_HTML_NEW = ('id="stagePicker" id="stageFile" id="stageUploadBtn" id="stageInstallBtn" id="stageClearBtn" '
                '/api/update/stage id="picoRollbackBtn" id="recoveryExitBox" style="display:none" id="recoveryExitBtn"')


class OtaStageCard(unittest.TestCase):
    def test_03_current_page(self):
        c = ctx_for(pages={"/ota": OTA_HTML_NEW}, _results={"OT-E01": CaseResult(PASS)})
        self.assertEqual(run("WEB-OTA-03", c).verdict, PASS)

    def test_03_fails_when_retired_control_returns_or_stage_missing(self):
        c = ctx_for(pages={"/ota": OTA_HTML_NEW + ' id="espUpdateBtn"'}, _results={"OT-E01": CaseResult(PASS)})
        self.assertEqual(run("WEB-OTA-03", c).verdict, FAIL)
        c = ctx_for(pages={"/ota": "x"}, _results={"OT-E01": CaseResult(PASS)})
        self.assertEqual(run("WEB-OTA-03", c).verdict, FAIL)

    def test_04_static_only(self):
        self.assertEqual(run("WEB-OTA-04", ctx_for(pages={"/ota": OTA_HTML_NEW})).verdict, PASS)
        c = ctx_for(pages={"/ota": OTA_HTML_NEW + " /api/ota/esp/rollback"})
        self.assertEqual(run("WEB-OTA-04", c).verdict, FAIL)


class OtaAlias(unittest.TestCase):
    def test_not_run_and_inconclusive(self):
        self.assertEqual(run("WEB-OTA-03", ctx_for(pages={"/ota": OTA_HTML_NEW})).verdict, NR)
        c = ctx_for(pages={"/ota": OTA_HTML_NEW}, _results={"OT-E01": CaseResult(SKIP, reason="no image")})
        self.assertEqual(run("WEB-OTA-03", c).verdict, INC)

    def test_fail_target_failed(self):
        c = ctx_for(pages={"/ota": OTA_HTML_NEW}, _results={"OT-E01": CaseResult(FAIL, reason="x")})
        self.assertEqual(run("WEB-OTA-03", c).verdict, FAIL)


class Ota0508(unittest.TestCase):
    def test_05(self):
        pg = {"/ota": OTA_HTML}
        self.assertEqual(run("WEB-OTA-05", ctx_for({"/api/ota/esp/status": {"recovery_mode": False}}, pg)).verdict, PASS)
        self.assertEqual(run("WEB-OTA-05", ctx_for({"/api/ota/esp/status": {"recovery_mode": True}}, pg)).verdict, INC)
        self.assertEqual(run("WEB-OTA-05", ctx_for({"/api/ota/esp/status": {"recovery_mode": False}},
                                                   {"/ota": "recoveryExitBtn"})).verdict, FAIL)

    def test_06(self):
        self.assertEqual(run("WEB-OTA-06", ctx_for(pages={"/ota": OTA_HTML})).verdict, PASS)
        self.assertEqual(run("WEB-OTA-06", ctx_for(pages={"/ota": "x"})).verdict, FAIL)
        self.assertEqual(run("WEB-OTA-06", ctx_for()).verdict, INC)

    def test_07(self):
        pg = {"/ota": OTA_HTML}
        self.assertEqual(run("WEB-OTA-07", ctx_for({"/api/status": {}}, pg)).verdict, PASS)
        self.assertEqual(run("WEB-OTA-07", ctx_for({"/api/status": {"boot_button_bypass_active": True}}, pg)).verdict, FAIL)
        self.assertEqual(run("WEB-OTA-07", ctx_for({"/api/status": {}}, {"/ota": '<div id="bypassBanner">'})).verdict, FAIL)

    def test_08(self):
        ok = {"protocol_version_known": True, "protocol_compatible": True, "protocol_version": 16}
        self.assertEqual(run("WEB-OTA-08", ctx_for({"/api/ota/pico/status": ok})).verdict, PASS)
        self.assertEqual(run("WEB-OTA-08", ctx_for({"/api/ota/pico/status": dict(ok, protocol_compatible=False)})).verdict, FAIL)
        self.assertEqual(run("WEB-OTA-08", ctx_for({"/api/ota/pico/status": {"protocol_version_known": False}})).verdict, INC)


if __name__ == "__main__":
    unittest.main()
