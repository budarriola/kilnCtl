"""Fake-board tests for cases_web_misc (WEB-WIFI/SEC/BAK/KCFG/LOG/X)."""
import json
import tempfile
import unittest
from pathlib import Path

from kilnctrl.bench_test import cases_web_misc as M
from kilnctrl.bench_test import registry as R
from kilnctrl.bench_test.registry import CaseResult, Verdict
from kilnctrl.bench_test.cases_web_rw import ForbiddenWrite, _post_json
from test_bench_test_cases_web_diag import IDLE, ctx_for, run

PASS, FAIL, INC, SKIP, NR = (Verdict.PASS, Verdict.FAIL, Verdict.INCONCLUSIVE, Verdict.SKIP, Verdict.NOT_RUN)

HOME = {"mode": "home", "sta_connected": True, "ssid": "Bench", "ip_mode": "dhcp"}


class Wifi02(unittest.TestCase):
    def test_pass(self):
        nets = [{"ssid": "Bench", "saved": True, "in_range": True, "connected": True}]
        self.assertEqual(run("WEB-WIFI-02", ctx_for({"/status": HOME, "/networks": nets})).verdict, PASS)

    def test_fail_not_saved(self):
        nets = [{"ssid": "Bench", "saved": False, "in_range": True, "connected": True}]
        self.assertEqual(run("WEB-WIFI-02", ctx_for({"/status": HOME, "/networks": nets})).verdict, FAIL)

    def test_inconclusive_not_home(self):
        self.assertEqual(run("WEB-WIFI-02", ctx_for({"/status": {"mode": "ap"}})).verdict, INC)


WIFI_HTML = ('id="apSection" #apSection { display: none; } id="apQrCanvas" var KilnQr function renderApQr( '
             "if (s.mode === 'ap') renderApQr "
             'ipModeDhcpBtn ipModeStaticBtn <div id="staticIpFields" style="display:none"></div> applyIpModeUi '
             "document.getElementById('ipModeStaticBtn').addEventListener('click', function(){ applyIpModeUi('static'); });")


class Wifi0304(unittest.TestCase):
    def test_03(self):
        self.assertEqual(run("WEB-WIFI-03", ctx_for({"/status": HOME}, {"/wifi": WIFI_HTML})).verdict, PASS)
        self.assertEqual(run("WEB-WIFI-03", ctx_for({"/status": HOME},
                                                    {"/wifi": WIFI_HTML.replace('id="apQrCanvas"', "")})).verdict, FAIL)
        self.assertEqual(run("WEB-WIFI-03", ctx_for({"/status": {"mode": "ap"}}, {"/wifi": WIFI_HTML})).verdict, INC)

    def test_04(self):
        self.assertEqual(run("WEB-WIFI-04", ctx_for({"/status": HOME}, {"/wifi": WIFI_HTML})).verdict, PASS)
        self.assertEqual(run("WEB-WIFI-04", ctx_for({"/status": dict(HOME, ip_mode="weird")},
                                                    {"/wifi": WIFI_HTML})).verdict, FAIL)
        bad = WIFI_HTML.replace("applyIpModeUi('static');", "fetch('x');")
        self.assertEqual(run("WEB-WIFI-04", ctx_for({"/status": HOME}, {"/wifi": bad})).verdict, FAIL)
        self.assertEqual(run("WEB-WIFI-04", ctx_for({}, {"/wifi": WIFI_HTML})).verdict, INC)


class Wifi05(unittest.TestCase):
    def dir_with(self, body):
        d = tempfile.mkdtemp()
        Path(d, "x.py").write_text(body, encoding="utf-8")
        return d

    def test_pass_real_sources(self):
        r = run("WEB-WIFI-05", ctx_for({"/status": HOME}))
        self.assertEqual(r.verdict, PASS, r.reason)

    def test_guard_raises(self):
        with self.assertRaises(ForbiddenWrite):
            _post_json(ctx_for(), "/forget", {})
        self.assertEqual(_post_json(ctx_for(), "/api/zones", {})[0], 200)

    def test_fail_source_literal(self):
        d = self.dir_with('X = "/provision"\n')
        self.assertEqual(run("WEB-WIFI-05", ctx_for({"/status": HOME}, bench_test_dir=d)).verdict, FAIL)

    def test_fail_forbidden_import(self):
        d = self.dir_with("import wifi_uart\n")
        self.assertEqual(run("WEB-WIFI-05", ctx_for({"/status": HOME}, bench_test_dir=d)).verdict, FAIL)

    def test_denylist_constant_and_docstring_exempt(self):
        d = self.dir_with('"""talks about /forget"""\n_WIFI_WRITE_DENYLIST = {"p": ("/forget",)}\n')
        self.assertEqual(run("WEB-WIFI-05", ctx_for({"/status": HOME}, bench_test_dir=d)).verdict, PASS)

    def test_fail_status_changed(self):
        c = ctx_for({"/status": HOME}, _wifi_status_start=("ap", "x", "Bench", "dhcp"))
        self.assertEqual(run("WEB-WIFI-05", c).verdict, FAIL)

    def test_inconclusive_when_wifi06_ran(self):
        c = ctx_for({"/status": HOME}, _results={"WEB-WIFI-06": CaseResult(PASS)})
        self.assertEqual(run("WEB-WIFI-05", c).verdict, INC)


SEC_HTML = ('id="kcSecWebEnabled" kcSecPolicySave Set both passwords before enabling web sign-in. '
            'id="kcSecClearCreds" Clear login credentials window.kcConfirm cmd clear_credentials')


class Sec(unittest.TestCase):
    def test_02(self):
        cfg = {"admin_password_set": True, "user_password_set": False}
        self.assertEqual(run("WEB-SEC-02", ctx_for({"/api/auth/config": cfg}, {"/settings/security": SEC_HTML})).verdict, PASS)
        h = SEC_HTML.replace("Set both passwords before enabling web sign-in.", "")
        self.assertEqual(run("WEB-SEC-02", ctx_for({"/api/auth/config": cfg}, {"/settings/security": h})).verdict, FAIL)
        self.assertEqual(run("WEB-SEC-02", ctx_for({}, {"/settings/security": SEC_HTML})).verdict, INC)

    def test_06(self):
        pg = {"/settings/security": SEC_HTML}
        clean = tempfile.mkdtemp()
        c = ctx_for({"/api/auth/config": {"admin_password_set": True}}, pg, bench_test_dir=clean,
                    _sec06_admin_start=True)
        self.assertEqual(run("WEB-SEC-06", c).verdict, PASS)
        c = ctx_for({"/api/auth/config": {"admin_password_set": False}}, pg, bench_test_dir=clean,
                    _sec06_admin_start=True)
        self.assertEqual(run("WEB-SEC-06", c).verdict, FAIL)
        c = ctx_for({"/api/auth/config": {"admin_password_set": True}}, {"/settings/security": SEC_HTML.replace("window.kcConfirm", "")},
                    bench_test_dir=clean)
        self.assertEqual(run("WEB-SEC-06", c).verdict, FAIL)
        self.assertEqual(run("WEB-SEC-06", ctx_for({}, pg, bench_test_dir=clean)).verdict, INC)

    def test_06_source_hit(self):
        d = tempfile.mkdtemp()
        Path(d, "y.py").write_text('F = {"cmd": "clear_credentials"}\n', encoding="utf-8")
        c = ctx_for({"/api/auth/config": {"admin_password_set": True}}, {"/settings/security": SEC_HTML}, bench_test_dir=d)
        self.assertEqual(run("WEB-SEC-06", c).verdict, FAIL)


def _export(zones=2, name="p"):
    return {"kind": "kilnctl_backup", "version": 1, "profiles": [{"id": 0, "name": name}],
            "zones": [{"index": i} for i in range(zones)]}


def _bak_ctx(exports, zones_n=2, **extra):
    it = iter(exports)

    def exp():
        p = next(it)
        return json.dumps(p), p

    r = dict(IDLE, **{"/api/zones": {"thermo_count": zones_n},
                      "/api/profiles": {"profiles": [{"id": 0, "builtin": False}, {"id": 1, "builtin": True}]}})
    return ctx_for(r, backup_export=exp, sleep=lambda s: None, **extra)


class Bak(unittest.TestCase):
    def test_02(self):
        self.assertEqual(run("WEB-BAK-02", _bak_ctx([_export()])).verdict, PASS)
        self.assertEqual(run("WEB-BAK-02", _bak_ctx([_export(zones=1)])).verdict, FAIL)
        self.assertEqual(run("WEB-BAK-02", ctx_for({"/api/zones": (500, None)})).verdict, INC)

    def test_02_sensitive(self):
        e = _export()
        e["wifi_password"] = "x"
        self.assertEqual(run("WEB-BAK-02", _bak_ctx([e])).verdict, FAIL)

    def test_03(self):
        self.assertEqual(run("WEB-BAK-03", _bak_ctx([_export(), _export()])).verdict, PASS)
        self.assertEqual(run("WEB-BAK-03", _bak_ctx([_export(), _export(name="q")])).verdict, FAIL)

    def test_03_inconclusive_busy(self):
        c = _bak_ctx([_export(), _export()])
        c["http_get_json"] = lambda p: (200, {"state": "running"})
        self.assertEqual(run("WEB-BAK-03", c).verdict, INC)

    def test_04(self):
        t = "<p>Import is refused while a profile is running / heaters on / OTA in progress</p>"
        self.assertEqual(run("WEB-BAK-04", ctx_for(pages={"/settings/backup": t})).verdict, PASS)
        self.assertEqual(run("WEB-BAK-04", ctx_for(pages={"/settings/backup": "x"})).verdict, FAIL)


KC_HTML = ('kcApplyBtn apply_status kcAckHwDiffers X-Kiln-Ack-Hardware-Differs '
           '<div id="kcDivergeBanner" hidden></div> refreshDivergence safety_ceiling_match')


class FakeKcfg:
    """A kiln-config store behind the ctx seams."""

    def __init__(self, leak=False):
        self.cfgs = {1: "main"}
        self.active = 1
        self.next = 10
        self.leak = leak
        self.deleted = []
        self.posts = []

    def snapshot(self):
        return {"active_id": self.active, "max_count": 8,
                "configs": [{"id": i, "name": n, "is_active": i == self.active} for i, n in self.cfgs.items()]}

    def get(self, path):
        if path == "/api/kiln_configs":
            return 200, self.snapshot()
        return (200, IDLE[path]) if path in IDLE else (404, None)

    def post(self, path, f):
        self.posts.append(path)
        if path.endswith("/save"):
            self.cfgs[self.next] = f["name"]
            self.active = self.next  # firmware: save activates the new entry
            self.next += 1
            return 200, {"id": self.next - 1}
        if path.endswith("/clone"):
            self.cfgs[self.next] = f["name"]
            self.next += 1
            return 200, {"id": self.next - 1}
        if path.endswith("/rename"):
            self.cfgs[int(f["id"])] = f["name"]
            return 200, {"ok": True}
        if path.endswith("/delete"):
            self.deleted.append(int(f["id"]))
            if int(f["id"]) == self.active:
                return 409, None  # firmware refuses to delete the active config
            if not self.leak:
                self.cfgs.pop(int(f["id"]), None)
            return 200, {"ok": True}
        return 404, None

    def text(self, path):
        # Envelope per kiln_package.c:250-266 (kiln_package_export_json).
        return 200, json.dumps({"kind": "kilnctl-kiln-config", "pkg_schema": 1, "name": "BENCH_tmp_r",
                                "esp_blob_len": 0, "esp_blob_hex": "", "source_board_id": "0x00000000",
                                "pico": [], "pkg_hash": "0x00000000"})

    def body(self, path, pkg):
        assert pkg["name"] == "BENCH_tmp_i" and "esp_blob_hex" in pkg  # case renames the envelope's top-level name
        self.cfgs[self.next] = pkg["name"]
        self.next += 1
        return 200, json.dumps({"id": self.next - 1})

    def ctx(self):
        return {"http_get_json": self.get, "http_post_json": self.post, "http_get_text": self.text,
                "http_post_json_body": self.body, "web_client": None}


class Kcfg(unittest.TestCase):
    def test_02_pass(self):
        f = FakeKcfg()
        r = run("WEB-KCFG-02", f.ctx())
        self.assertEqual(r.verdict, PASS, r.reason)
        self.assertEqual(sorted(f.deleted), [10, 11])
        self.assertNotIn(1, f.deleted)
        self.assertFalse([p for p in f.posts if p.endswith("/save")])
        self.assertEqual(f.active, 1)

    def test_02_restore_mismatch_fails(self):
        r = run("WEB-KCFG-02", FakeKcfg(leak=True).ctx())
        self.assertEqual(r.verdict, FAIL)
        self.assertIn("restore mismatch", r.reason)

    def test_02_gate_few_slots(self):
        f = FakeKcfg()
        f.cfgs.update({2: "a", 3: "b", 4: "c", 5: "d", 6: "e", 7: "f"})
        self.assertEqual(run("WEB-KCFG-02", f.ctx()).verdict, INC)

    def test_03(self):
        ok = {"state": "idle", "id": None, "diverged": False, "reason": ""}
        pg = {"/settings/kiln_configs": KC_HTML}
        self.assertEqual(run("WEB-KCFG-03", ctx_for({"/api/kiln_configs/apply_status": ok}, pg)).verdict, PASS)
        self.assertEqual(run("WEB-KCFG-03", ctx_for({"/api/kiln_configs/apply_status": {"state": "bogus"}}, pg)).verdict, FAIL)
        self.assertEqual(run("WEB-KCFG-03", ctx_for({"/api/kiln_configs/apply_status": dict(ok, state="running")}, pg)).verdict, INC)

    def test_04(self):
        self.assertEqual(run("WEB-KCFG-04", ctx_for(pages={"/settings/kiln_configs": KC_HTML})).verdict, PASS)
        h = KC_HTML.replace("X-Kiln-Ack-Hardware-Differs", "")
        self.assertEqual(run("WEB-KCFG-04", ctx_for(pages={"/settings/kiln_configs": h})).verdict, FAIL)

    def test_05(self):
        pg = {"/settings/kiln_configs": KC_HTML}

        def rd(s):
            return {"/api/readiness": {"items": [{"key": "safety_ceiling_match", "status": s}]}}
        self.assertEqual(run("WEB-KCFG-05", ctx_for(rd("ok"), pg)).verdict, PASS)
        self.assertEqual(run("WEB-KCFG-05", ctx_for(rd("not_done"), pg)).verdict, FAIL)
        self.assertEqual(run("WEB-KCFG-05", ctx_for(rd("cannot_yet"), pg)).verdict, INC)
        self.assertEqual(run("WEB-KCFG-05", ctx_for({"/api/readiness": {"items": []}}, pg)).verdict, FAIL)


class FakeSec:
    def __init__(self, bad=(401, None), good=(200, "kiln_sid=x"), sessions=None, restore_ok=True):
        self.orig = {"web_enabled": False, "lcd_enabled": False, "web_timeout_min": 30, "lcd_timeout_min": 10}
        self.cfg = dict(self.orig)
        self.bad, self.good = bad, good
        self.sessions = list(sessions or [])
        self.restore_ok = restore_ok
        self.policies = []
        self.pw_writes = 0
        self.last_login_set_cookie = {"httponly": True, "secure": False, "samesite": "Strict"}

    def get_config(self):
        return 200, dict(self.cfg)

    def set_web_password(self, u, p):
        self.pw_writes += 1
        return 200, {"ok": True}

    def set_policy(self, we, le, wt, lt):
        self.policies.append((we, le, wt, lt))
        if we == self.orig["web_enabled"] and not self.restore_ok:
            return 200, {"ok": True}
        self.cfg = {"web_enabled": we, "lcd_enabled": le, "web_timeout_min": wt, "lcd_timeout_min": lt}
        return 200, {"ok": True}

    def login(self, u, p):
        return self.bad if p == "bench-wrong-credential-x" else self.good

    def get_session(self, cookie):
        return 200, (self.sessions.pop(0) if len(self.sessions) > 1 else self.sessions[0])

    def extend_session(self, cookie):
        return 200, {"ok": True}


def _auth_ctx(client, **extra):
    c = ctx_for(IDLE, sec_client=client, web_username="u", web_password="p", sleep=lambda s: None, **extra)
    return c


class LogX(unittest.TestCase):
    def test_log02_x02_no_password_write_when_login_fails(self):
        for case in ("WEB-LOG-02", "WEB-X-02"):
            f = FakeSec(good=(401, None), sessions=[{"prompt": False, "seconds_left": 40, "role": "admin"}])
            c = _auth_ctx(f)
            c["http_get_text"] = lambda p: (200, "kc-lock-prompt Stay unlocked fetch('/api/auth/session/extend' st.prompt")
            r = run(case, c)
            self.assertEqual(r.verdict, INC, (case, r.reason))
            self.assertEqual(f.pw_writes, 0, case)
            self.assertNotIn("p", (r.reason or "").split())

    def test_log02_pass(self):
        f = FakeSec(sessions=[{"role": "admin"}])
        self.assertEqual(run("WEB-LOG-02", _auth_ctx(f)).verdict, PASS)
        self.assertEqual(f.cfg, f.orig)

    def test_log02_fail_no_httponly(self):
        f = FakeSec(sessions=[{"role": "admin"}])
        f.last_login_set_cookie = {"httponly": False, "secure": False, "samesite": "Strict"}
        r = run("WEB-LOG-02", _auth_ctx(f))
        self.assertEqual(r.verdict, FAIL)
        self.assertIn("HttpOnly", r.reason)

    def test_real_client_exposes_cookie_flags_not_value(self):
        from email.message import Message
        from unittest import mock
        from kilnctrl.bench_test import cases_web_rw as RW
        h = Message()
        h["Set-Cookie"] = "kiln_sid=abc123; HttpOnly; SameSite=Strict; Path=/"
        with mock.patch.object(RW, "_http_post_raw", return_value=(200, "{}", h)):
            c = RW._SecHttpClient("h")
            st, ck = c.login("u", "p")
        self.assertEqual(ck, "abc123")
        self.assertEqual(c.last_login_set_cookie, {"httponly": True, "secure": False, "samesite": "Strict"})
        self.assertNotIn("abc123", repr(c.last_login_set_cookie))
        h2 = Message()
        h2["Set-Cookie"] = "kiln_sid=abc123; Path=/"
        with mock.patch.object(RW, "_http_post_raw", return_value=(200, "{}", h2)):
            c.login("u", "p")
        self.assertFalse(c.last_login_set_cookie["httponly"])

    def test_log02_fail_bad_login_cookie(self):
        f = FakeSec(bad=(200, "kiln_sid=y"), sessions=[{"role": "admin"}])
        self.assertEqual(run("WEB-LOG-02", _auth_ctx(f)).verdict, FAIL)
        self.assertEqual(f.cfg, f.orig)

    def test_log02_restore_mismatch(self):
        f = FakeSec(sessions=[{"role": "admin"}], restore_ok=False)
        r = run("WEB-LOG-02", _auth_ctx(f))
        self.assertEqual(r.verdict, FAIL)
        self.assertIn("restore mismatch", r.reason)

    def test_log02_inconclusive_429_and_skip(self):
        f = FakeSec(bad=(429, None), sessions=[{"role": "admin"}])
        self.assertEqual(run("WEB-LOG-02", _auth_ctx(f)).verdict, INC)
        c = ctx_for(IDLE, sec_client=FakeSec(sessions=[{}]), web_username=None, web_password=None)
        import os
        saved = {k: os.environ.pop(k, None) for k in ("KILNCTL_WEB_USERNAME", "KILNCTL_WEB_PASSWORD")}
        try:
            self.assertEqual(run("WEB-LOG-02", c).verdict, SKIP)
        finally:
            for k, v in saved.items():
                if v is not None:
                    os.environ[k] = v

    def test_log03(self):
        self.assertEqual(run("WEB-LOG-03", {"_results": {"WEB-SEC-05": CaseResult(PASS, observed={"c": 1})}}).verdict, PASS)
        self.assertEqual(run("WEB-LOG-03", {"_results": {"WEB-SEC-05": CaseResult(FAIL, reason="r")}}).verdict, FAIL)
        self.assertEqual(run("WEB-LOG-03", {"_results": {"WEB-SEC-05": CaseResult(INC, reason="r")}}).verdict, INC)
        self.assertEqual(run("WEB-LOG-03", {"_results": {}}).verdict, NR)

    APP_JS = "kc-lock-prompt Stay unlocked fetch('/api/auth/session/extend' st.prompt"

    def x02(self, sessions):
        f = FakeSec(sessions=sessions)
        c = _auth_ctx(f)
        c["web_client"].pages["/app.js"] = self.APP_JS
        return run("WEB-X-02", c), f

    def test_x02_pass(self):
        r, f = self.x02([{"prompt": False, "seconds_left": 40, "role": "admin"},
                         {"prompt": True, "seconds_left": 8, "role": "admin"},
                         {"prompt": False, "seconds_left": 59, "role": "admin"}])
        self.assertEqual(r.verdict, PASS, r.reason)
        self.assertEqual(f.cfg, f.orig)

    def test_x02_fail_post_extend(self):
        r, _ = self.x02([{"prompt": False, "seconds_left": 40, "role": "admin"},
                         {"prompt": True, "seconds_left": 8, "role": "admin"},
                         {"prompt": False, "seconds_left": 0, "role": "none"}])
        self.assertEqual(r.verdict, FAIL)

    def test_x02_fail_missing_marker(self):
        f = FakeSec(sessions=[{}])
        c = _auth_ctx(f)
        c["web_client"].pages["/app.js"] = "x"
        self.assertEqual(run("WEB-X-02", c).verdict, FAIL)


if __name__ == "__main__":
    unittest.main()


class RunStartWiring(unittest.TestCase):
    def test_probes_wired(self):
        from kilnctrl.bench_test.registry import get_case
        self.assertEqual(get_case("WEB-WIFI-05").run_start_probe[0], "_wifi_status_start")
        self.assertEqual(get_case("WEB-SEC-06").run_start_probe[0], "_sec06_admin_start")
        key, fn = get_case("WEB-SEC-06").run_start_probe
        self.assertIs(fn(ctx_for({"/api/auth/config": {"admin_password_set": True}})), True)
        self.assertIsNone(fn(ctx_for({})))
        key, fn = get_case("WEB-WIFI-05").run_start_probe
        s = {"mode": "sta", "state": "up", "ssid": "x", "ip_mode": "dhcp"}
        self.assertEqual(fn(ctx_for({"/status": s})), ("sta", "up", "x", "dhcp"))
