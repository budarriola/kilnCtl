#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_web_rw -- Wave 2's WEB
read/write round-trip cases (WEB-DASH-13, WEB-DIAG-07, WEB-DIAG-08) and
WEB-SEC-03 (auth on/off). All board access is faked via ctx overrides;
nothing here touches urllib or hardware.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_web_rw.py -q
"""
from __future__ import annotations

import gzip
import io
import os
import sys
import unittest
import urllib.error
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_web_rw as C  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, Verdict  # noqa: E402


# ---------------------------------------------------------------------------
# _SecHttpClient._get -- Accept-Encoding: gzip header + gzip decode
# (2026-09-24 review follow-up: nothing previously exercised this at the
# urllib layer -- every existing WEB-SEC-03 test injects a fake client via
# ctx["sec_client"] and never goes near urllib.request.urlopen, so dropping
# the header entirely left every test green.)
# ---------------------------------------------------------------------------

class _FakeHeaders(dict):
    def get_all(self, name, default=None):
        v = self.get(name)
        return [v] if v is not None else default


class _FakeHttpResponse:
    def __init__(self, status, body_bytes, headers):
        self._status = status
        self._body = body_bytes
        self.headers = _FakeHeaders(headers)

    def getcode(self):
        return self._status

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class SecHttpClientGetTest(unittest.TestCase):
    def test_sends_accept_encoding_gzip(self):
        client = C._SecHttpClient("1.2.3.4")
        captured = {}

        def fake_open(req, timeout=None):
            captured["header"] = req.get_header("Accept-encoding")
            return _FakeHttpResponse(200, b"plain body", {})

        with mock.patch.object(client._opener, "open", side_effect=fake_open):
            status, text, _headers = client._get("/")
        self.assertEqual(captured["header"], "gzip")
        self.assertEqual(status, 200)
        self.assertEqual(text, "plain body")

    def test_decodes_gzip_content_encoding(self):
        client = C._SecHttpClient("1.2.3.4")
        payload = gzip.compress(b"<html>hello</html>")

        def fake_open(req, timeout=None):
            return _FakeHttpResponse(200, payload, {"Content-Encoding": "gzip"})

        with mock.patch.object(client._opener, "open", side_effect=fake_open):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 200)
        self.assertEqual(text, "<html>hello</html>")

    def test_malformed_gzip_body_does_not_crash_and_returns_no_body(self):
        """Advisory fix: a Content-Encoding: gzip header on a body that
        doesn't actually decompress must not raise out of _get()."""
        client = C._SecHttpClient("1.2.3.4")

        def fake_open(req, timeout=None):
            return _FakeHttpResponse(200, b"not actually gzip", {"Content-Encoding": "gzip"})

        with mock.patch.object(client._opener, "open", side_effect=fake_open):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 200)
        self.assertIsNone(text)

    def test_http_error_branch_decodes_gzip_body_and_reports_status(self):
        client = C._SecHttpClient("1.2.3.4")
        payload = gzip.compress(b'{"ok": false, "reason": "forbidden"}')
        headers = _FakeHeaders({"Content-Encoding": "gzip"})

        def fake_open(req, timeout=None):
            raise urllib.error.HTTPError(
                url="http://1.2.3.4/", code=403, msg="Forbidden", hdrs=headers, fp=io.BytesIO(payload)
            )

        with mock.patch.object(client._opener, "open", side_effect=fake_open):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 403)
        self.assertEqual(text, '{"ok": false, "reason": "forbidden"}')

    def test_http_error_branch_without_gzip_reports_plain_body(self):
        client = C._SecHttpClient("1.2.3.4")
        headers = _FakeHeaders({})

        def fake_open(req, timeout=None):
            raise urllib.error.HTTPError(
                url="http://1.2.3.4/", code=401, msg="Unauthorized", hdrs=headers, fp=io.BytesIO(b"nope")
            )

        with mock.patch.object(client._opener, "open", side_effect=fake_open):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 401)
        self.assertEqual(text, "nope")

    def test_302_is_returned_as_302_and_not_followed(self):
        """The WEB-SEC-03 bug: firmware answers an unauthenticated
        non-/api GET with 302 + Location: /login?return=... . A client that
        auto-follows lands on the login shell with 200 -- which would make a
        redirecting page shell look open (page_shell_open) when it is not.
        This client must observe the 302 itself."""
        client = C._SecHttpClient("1.2.3.4")
        headers = _FakeHeaders({"Location": "/login?return=/settings/zones"})

        def fake_open(req, timeout=None):
            # A real OpenerDirector with _NoRedirectHandler installed hands
            # back the original 302 response rather than raising or
            # re-issuing the request against Location -- this fake mirrors
            # that contract directly.
            return _FakeHttpResponse(302, b"", headers)

        with mock.patch.object(client._opener, "open", side_effect=fake_open):
            status, location = client.get_status_location("/settings/zones")
        self.assertEqual(status, 302)
        self.assertEqual(location, "/login?return=/settings/zones")

    def test_no_redirect_handler_returns_none_from_redirect_request(self):
        """Direct check of the handler installed on client._opener: it must
        refuse to build a follow-up request for any 3xx."""
        handler = C._NoRedirectHandler()
        result = handler.redirect_request(
            req=mock.Mock(), fp=mock.Mock(), code=302, msg="Found",
            headers=_FakeHeaders({"Location": "/login"}), newurl="http://1.2.3.4/login",
        )
        self.assertIsNone(result)


# ---------------------------------------------------------------------------
# judge_web_rw_toggle -- direct unit tests
# ---------------------------------------------------------------------------

class JudgeWebRwToggleTest(unittest.TestCase):
    def test_clean_round_trip_passes(self):
        result = J.judge_web_rw_toggle("f", "C", "F", True, "F", True, "C")
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_restore_mismatch_fails_unconditionally(self):
        """Even if the write path looked perfect, a restore that did not
        round-trip must FAIL -- board left holding the test value is a
        hazard this suite must never paper over."""
        result = J.judge_web_rw_toggle("f", "C", "F", True, "F", True, "F")
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_restore_post_not_ok_fails(self):
        result = J.judge_web_rw_toggle("f", "C", "F", True, "F", False, "C")
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_write_did_not_take_effect_fails(self):
        result = J.judge_web_rw_toggle("f", "C", "F", False, "C", True, "C")
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("write", result.reason)


# ---------------------------------------------------------------------------
# judge_web_sec03 -- direct unit tests
# ---------------------------------------------------------------------------

class JudgeWebSec03Test(unittest.TestCase):
    def _happy(self, **overrides):
        kwargs = dict(
            pw_ok=True, enabled_ok=True, dashboard_ok=True, page_shell_open=True,
            api_route_gated=True,
            login_ok=True, session_ok=True, extend_ok=True, restore_ok=True, restore_matches=True,
        )
        kwargs.update(overrides)
        return kwargs

    def test_happy_path_passes(self):
        result = J.judge_web_sec03(**self._happy())
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_restore_failure_fails_unconditionally_even_if_everything_else_passed(self):
        result = J.judge_web_sec03(**self._happy(restore_ok=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_restore_mismatch_fails(self):
        result = J.judge_web_sec03(**self._happy(restore_matches=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_password_not_confirmed_fails(self):
        result = J.judge_web_sec03(**self._happy(pw_ok=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("set_web_password", result.reason)

    def test_page_shell_not_open_fails(self):
        result = J.judge_web_sec03(**self._happy(page_shell_open=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("settings/zones", result.reason)

    def test_api_route_not_gated_fails_even_if_page_route_is_gated(self):
        """The stale-premise bug this fix closes: a 200 on the data-bearing
        GET /api/zones must FAIL even when /settings/zones correctly
        answered as an open page shell -- one probe is not evidence for the
        other."""
        result = J.judge_web_sec03(**self._happy(api_route_gated=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("api/zones", result.reason)


# ---------------------------------------------------------------------------
# WEB-DASH-13 / WEB-DIAG-07 / WEB-DIAG-08 -- case-function tests with a fake
# HTTP seam (ctx["http_get_json"] / ctx["http_post_json"]).
# ---------------------------------------------------------------------------

class FakeHttp:
    """Sequenced GET/POST fake keyed by path; each entry is a queue of
    responses so a case's repeated GETs to the same path see original,
    then written, then restored."""

    def __init__(self, get_queues: dict, post_responses: dict):
        self._get_queues = {k: list(v) for k, v in get_queues.items()}
        self._post_responses = post_responses
        self.post_calls = []

    def get(self, path):
        q = self._get_queues.get(path)
        if not q:
            return 404, None
        return q.pop(0)

    def post(self, path, fields):
        self.post_calls.append((path, dict(fields)))
        resp = self._post_responses.get(path)
        if callable(resp):
            return resp(fields)
        return resp if resp is not None else (404, None)


class Dash13Test(unittest.TestCase):
    def test_round_trip_passes(self):
        fake = FakeHttp(
            get_queues={"/api/status": [
                (200, {"temp_unit": "C"}),
                (200, {"temp_unit": "F"}),
                (200, {"temp_unit": "C"}),
            ]},
            post_responses={"/api/unit_pref": (200, {"ok": True})},
        )
        ctx = {"suite": "web", "http_get_json": fake.get, "http_post_json": fake.post}
        result = C._case_dash13(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_write_did_not_take_effect_fails_but_restore_ran(self):
        fake = FakeHttp(
            get_queues={"/api/status": [
                (200, {"temp_unit": "C"}),
                (200, {"temp_unit": "C"}),  # write did not take
                (200, {"temp_unit": "C"}),
            ]},
            post_responses={"/api/unit_pref": (200, {"ok": True})},
        )
        ctx = {"suite": "web", "http_get_json": fake.get, "http_post_json": fake.post}
        result = C._case_dash13(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("write", result.reason)
        # restore POST still happened
        self.assertEqual(len(fake.post_calls), 2)

    def test_restore_failure_fails_even_though_write_looked_fine(self):
        fake = FakeHttp(
            get_queues={"/api/status": [
                (200, {"temp_unit": "C"}),
                (200, {"temp_unit": "F"}),
                (200, {"temp_unit": "F"}),  # restore did not take
            ]},
            post_responses={"/api/unit_pref": (200, {"ok": True})},
        )
        ctx = {"suite": "web", "http_get_json": fake.get, "http_post_json": fake.post}
        result = C._case_dash13(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_exception_mid_write_still_restores(self):
        """finally-discipline: a raising http_post_json for the write must
        not prevent the restore POST from running."""
        calls = []

        def _get(path):
            return 200, {"temp_unit": "C"}

        def _post(path, fields):
            calls.append(fields)
            if len(calls) == 1:
                raise RuntimeError("simulated transport failure")
            return 200, {"ok": True}

        ctx = {"suite": "web", "http_get_json": _get, "http_post_json": _post}
        with self.assertRaises(RuntimeError):
            C._case_dash13(ctx)
        # restore POST attempted despite the exception
        self.assertEqual(len(calls), 2)

    def test_initial_get_failure_fails(self):
        ctx = {"suite": "web", "http_get_json": lambda path: (500, None), "http_post_json": lambda path, fields: (200, {"ok": True})}
        result = C._case_dash13(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Diag07Test(unittest.TestCase):
    def test_round_trip_passes(self):
        fake = FakeHttp(
            get_queues={"/api/watchdog_cfg": [
                (200, {"panic_disabled": False}),
                (200, {"panic_disabled": True}),
                (200, {"panic_disabled": False}),
            ]},
            post_responses={"/api/watchdog_cfg": (200, {"ok": True})},
        )
        ctx = {"suite": "web", "http_get_json": fake.get, "http_post_json": fake.post}
        result = C._case_diag07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        # POST form field is "disabled", not the JSON field "panic_disabled"
        self.assertEqual(fake.post_calls[0][1].get("disabled"), "1")


class Diag08Test(unittest.TestCase):
    def test_round_trip_passes(self):
        fake = FakeHttp(
            get_queues={"/api/ramp_assist": [
                (200, {"enabled": False}),
                (200, {"enabled": True}),
                (200, {"enabled": False}),
            ]},
            post_responses={"/api/ramp_assist": (200, {"ok": True})},
        )
        ctx = {"suite": "web", "http_get_json": fake.get, "http_post_json": fake.post}
        result = C._case_diag08(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)


# ---------------------------------------------------------------------------
# WEB-SEC-03 -- case-function tests via a fake ctx["sec_client"].
# ---------------------------------------------------------------------------

class PolicyFromConfigTest(unittest.TestCase):
    """Fix 3: a timeout of -1 (security_http_core.c's "never expire"
    sentinel) is a valid value that must pass through unchanged, never be
    coerced to the 30-minute default reserved for missing/invalid values."""

    def test_negative_one_timeout_passes_through_unchanged(self):
        cfg = {"web_enabled": True, "lcd_enabled": False, "web_timeout_min": -1, "lcd_timeout_min": -1}
        self.assertEqual(C._policy_from_config(cfg), {
            "web_enabled": True, "lcd_enabled": False, "web_timeout_min": -1, "lcd_timeout_min": -1,
        })

    def test_missing_timeout_defaults_to_30(self):
        cfg = {"web_enabled": False, "lcd_enabled": False}
        out = C._policy_from_config(cfg)
        self.assertEqual(out["web_timeout_min"], 30)
        self.assertEqual(out["lcd_timeout_min"], 30)

    def test_positive_timeout_passes_through(self):
        cfg = {"web_enabled": False, "lcd_enabled": False, "web_timeout_min": 15, "lcd_timeout_min": 45}
        out = C._policy_from_config(cfg)
        self.assertEqual(out["web_timeout_min"], 15)
        self.assertEqual(out["lcd_timeout_min"], 45)


class FakeSecClient:
    def __init__(self, web_enabled=False, lcd_enabled=False, web_timeout_min=30, lcd_timeout_min=30,
                 pw_ok=True, enable_ok=True, dashboard_status=200,
                 admin_page_status=200, admin_page_location=None,
                 api_status=401,
                 login_ok=True, session_ok=True, extend_status=200,
                 login_raises=False, restore_ok=True):
        self._cfg = {
            "web_enabled": web_enabled, "lcd_enabled": lcd_enabled,
            "web_timeout_min": web_timeout_min, "lcd_timeout_min": lcd_timeout_min,
        }
        self.pw_ok = pw_ok
        self.enable_ok = enable_ok
        self.dashboard_status = dashboard_status
        self.admin_page_status = admin_page_status
        self.admin_page_location = admin_page_location
        self.api_status = api_status
        self.login_ok = login_ok
        self.session_ok = session_ok
        self.extend_status = extend_status
        self.login_raises = login_raises
        self.restore_ok = restore_ok
        self.set_policy_calls = []

    def get_config(self):
        return 200, dict(self._cfg)

    def set_web_password(self, username, password):
        return (200, {"ok": True}) if self.pw_ok else (200, {"ok": False})

    def set_policy(self, web_enabled, lcd_enabled, web_timeout_min, lcd_timeout_min):
        self.set_policy_calls.append(web_enabled)
        if web_enabled and not self.enable_ok:
            return 200, {"ok": False}
        if not web_enabled and not self.restore_ok:
            return 200, {"ok": False}
        self._cfg["web_enabled"] = web_enabled
        return 200, {"ok": True}

    def login(self, username, password):
        if self.login_raises:
            raise RuntimeError("simulated login transport failure")
        return (200, "cookie123") if self.login_ok else (401, None)

    def get_session(self, cookie):
        return (200, {"role": "admin"}) if self.session_ok else (401, None)

    def extend_session(self, cookie):
        return self.extend_status, {}

    def get_status(self, path, cookie=None):
        if path == "/":
            return self.dashboard_status
        if path == "/api/zones":
            return self.api_status
        return self.admin_page_status

    def get_status_location(self, path, cookie=None):
        return self.admin_page_status, self.admin_page_location


class FakeSecClientWithBody(FakeSecClient):
    """Extends FakeSecClient with get_status_body, so _probe_dashboard takes
    the evidence-recording path (2026-09-24 WEB-SEC-03 fix) instead of the
    status-only fallback every plain FakeSecClient-based test above uses."""

    def __init__(self, dashboard_bodies=None, **kwargs):
        super().__init__(**kwargs)
        # A queue of (status, body) pairs consumed one per probe attempt;
        # once exhausted, repeats the last entry.
        self._bodies = list(dashboard_bodies) if dashboard_bodies else None
        self.get_status_body_calls = 0

    def get_status_body(self, path, cookie=None):
        self.get_status_body_calls += 1
        if path != "/":
            return self.admin_page_status, None
        if self._bodies:
            idx = min(self.get_status_body_calls - 1, len(self._bodies) - 1)
            return self._bodies[idx]
        return self.dashboard_status, "<html>dashboard</html>"


class FakeSec04Client:
    """Minimal fake for WEB-SEC-04: only the calls that case makes
    (get_config/set_lcd_pin/set_policy), tracking lcd_enabled so the
    restore-round-trip check is real rather than trivially true."""

    def __init__(self, admin_pin_set=False, web_enabled=False, lcd_enabled=False,
                 web_timeout_min=30, lcd_timeout_min=30,
                 set_lcd_pin_ok=True, enable_ok=True, restore_ok=True):
        self._cfg = {
            "web_enabled": web_enabled, "lcd_enabled": lcd_enabled,
            "web_timeout_min": web_timeout_min, "lcd_timeout_min": lcd_timeout_min,
            "admin_pin_set": admin_pin_set,
        }
        self.set_lcd_pin_ok = set_lcd_pin_ok
        self.enable_ok = enable_ok
        self.restore_ok = restore_ok
        self.set_lcd_pin_calls = []
        self.set_policy_calls = []

    def get_config(self):
        return 200, dict(self._cfg)

    def set_lcd_pin(self, role, pin):
        self.set_lcd_pin_calls.append((role, pin))
        return (200, {"ok": True}) if self.set_lcd_pin_ok else (200, {"ok": False})

    def set_policy(self, web_enabled, lcd_enabled, web_timeout_min, lcd_timeout_min):
        self.set_policy_calls.append(lcd_enabled)
        if lcd_enabled and not self.enable_ok:
            return 200, {"ok": False}
        if not lcd_enabled and not self.restore_ok:
            return 200, {"ok": False}
        self._cfg["web_enabled"] = web_enabled
        self._cfg["lcd_enabled"] = lcd_enabled
        self._cfg["web_timeout_min"] = web_timeout_min
        self._cfg["lcd_timeout_min"] = lcd_timeout_min
        return 200, {"ok": True}


class WebSec04Test(unittest.TestCase):
    _PIN = "1234"
    _WRONG = "1235"  # last digit flipped, matches _derive_wrong_lcd_pin("1234")

    def setUp(self):
        os.environ.pop(C._LCD_PIN_ENV, None)

    def tearDown(self):
        os.environ.pop(C._LCD_PIN_ENV, None)

    def test_skips_naming_the_variable_when_unset(self):
        client = FakeSec04Client(admin_pin_set=True)
        result = C._case_web_sec04({"suite": "web", "sec_client": client})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn(C._LCD_PIN_ENV, result.reason)
        self.assertEqual(client.set_lcd_pin_calls, [])
        self.assertEqual(client.set_policy_calls, [])

    def test_admin_pin_already_set_never_writes_but_still_passes(self):
        # admin_pin_set=True + var set (fix 2 branch): write nothing, hand
        # the PIN to LCD-19 -- only the enable/readback/restore round trip
        # is exercised, never a set_lcd_pin call.
        os.environ[C._LCD_PIN_ENV] = self._PIN
        client = FakeSec04Client(admin_pin_set=True, lcd_enabled=False)
        ctx = {"suite": "web", "sec_client": client}
        result = C._case_web_sec04(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(client.set_lcd_pin_calls, [])
        self.assertEqual(ctx["_lcd_pin"]["right_pin"], self._PIN)

    def test_happy_path_passes_and_restores(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN
        client = FakeSec04Client(admin_pin_set=False, lcd_enabled=False)
        ctx = {"suite": "web", "sec_client": client}
        result = C._case_web_sec04(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(client.set_lcd_pin_calls, [("admin", self._PIN)])
        # lcd_enabled must be back to its original (False) value afterward.
        self.assertEqual(client._cfg["lcd_enabled"], False)
        # A confirmed PASS hands the PIN pair (sourced from the env var, and
        # a derived wrong PIN that differs by exactly the last digit) to ctx
        # for LCD-19 -- never a hardcoded literal.
        self.assertEqual(ctx["_lcd_pin"], {"right_pin": self._PIN, "wrong_pin": self._WRONG})
        self.assertNotEqual(ctx["_lcd_pin"]["right_pin"], ctx["_lcd_pin"]["wrong_pin"])

    def test_ctx_literal_pin_used_without_env_var(self):
        # ctx["lcd_admin_pin"] takes priority over the environment.
        client = FakeSec04Client(admin_pin_set=False, lcd_enabled=False)
        ctx = {"suite": "web", "sec_client": client, "lcd_admin_pin": self._PIN}
        result = C._case_web_sec04(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(client.set_lcd_pin_calls, [("admin", self._PIN)])

    def test_set_lcd_pin_not_confirmed_fails_and_never_enables(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN
        client = FakeSec04Client(set_lcd_pin_ok=False)
        ctx = {"suite": "web", "sec_client": client}
        result = C._case_web_sec04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        # never enabled (True) -- the only set_policy call is the finally
        # block's restore-to-original (False, the fixture's original value).
        self.assertEqual(client.set_policy_calls, [False])
        self.assertNotIn("_lcd_pin", ctx)

    def test_enable_not_confirmed_fails(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN
        client = FakeSec04Client(enable_ok=False)
        ctx = {"suite": "web", "sec_client": client}
        result = C._case_web_sec04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertNotIn("_lcd_pin", ctx)

    def test_restore_failure_fails_even_if_everything_else_passed(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN
        client = FakeSec04Client(restore_ok=False)
        ctx = {"suite": "web", "sec_client": client}
        result = C._case_web_sec04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)
        self.assertNotIn("_lcd_pin", ctx)

    def test_initial_get_failure_fails(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN

        class BrokenClient:
            def get_config(self):
                return 500, None
        result = C._case_web_sec04({"suite": "web", "sec_client": BrokenClient()})
        self.assertEqual(result.verdict, Verdict.FAIL)


class WebSec04PinFormatAndLeakTest(unittest.TestCase):
    """The PIN is a real credential (KILNCTL_LCD_PIN): it must never reach a
    CaseResult's reason or observed (both land in summary.json/
    transcript.md), on any path, and a malformed value must be refused
    before it is used for anything."""

    _PIN = "8642097"  # distinctive, so a substring hit can't be a coincidence

    def setUp(self):
        os.environ.pop(C._LCD_PIN_ENV, None)

    def tearDown(self):
        os.environ.pop(C._LCD_PIN_ENV, None)

    def _assert_no_leak(self, result, *secrets):
        import json
        blob = (result.reason or "") + json.dumps(result.observed or {}, default=str)
        for secret in secrets:
            self.assertNotIn(secret, blob)

    def test_pin_never_in_result_on_any_path(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN
        wrong = C._derive_wrong_lcd_pin(self._PIN)
        variants = [
            dict(),
            dict(admin_pin_set=True),
            dict(set_lcd_pin_ok=False),
            dict(enable_ok=False),
            dict(restore_ok=False),
        ]
        for kw in variants:
            with self.subTest(**kw):
                result = C._case_web_sec04({"suite": "web", "sec_client": FakeSec04Client(**kw)})
                self._assert_no_leak(result, self._PIN, wrong)

    def test_leak_check_is_not_vacuous(self):
        # Negative control: the same check does catch a PIN that is present.
        from kilnctrl.bench_test.registry import CaseResult
        leaky = CaseResult(Verdict.FAIL, reason="x", observed={"pin": self._PIN})
        with self.assertRaises(AssertionError):
            self._assert_no_leak(leaky, self._PIN)

    def test_malformed_pin_fails_without_writing_or_echoing(self):
        for bad in ["98x7", "123", "123456789", "12 4", "\u0661\u0662\u0663\u0664"]:
            with self.subTest(bad=bad):
                os.environ[C._LCD_PIN_ENV] = bad
                client = FakeSec04Client(admin_pin_set=False)
                ctx = {"suite": "web", "sec_client": client}
                result = C._case_web_sec04(ctx)
                self.assertEqual(result.verdict, Verdict.FAIL)
                self.assertIn(C._LCD_PIN_ENV, result.reason)
                self._assert_no_leak(result, bad)
                self.assertEqual(client.set_lcd_pin_calls, [])
                self.assertEqual(client.set_policy_calls, [])
                self.assertNotIn("_lcd_pin", ctx)

    def test_never_timeout_passes_through_restore(self):
        os.environ[C._LCD_PIN_ENV] = self._PIN
        client = FakeSec04Client(web_timeout_min=-1, lcd_timeout_min=-1)
        result = C._case_web_sec04({"suite": "web", "sec_client": client})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(client._cfg["web_timeout_min"], -1)
        self.assertEqual(client._cfg["lcd_timeout_min"], -1)


class WebSec03Test(unittest.TestCase):
    def setUp(self):
        os.environ.pop("KILNCTL_WEB_USERNAME", None)
        os.environ.pop("KILNCTL_WEB_PASSWORD", None)

    def test_skips_without_credentials(self):
        client = FakeSecClient()
        result = C._case_web_sec03({"sec_client": client})
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_happy_path_passes_and_restores(self):
        client = FakeSecClient()
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        # enabled then restored to original (False)
        self.assertEqual(client.set_policy_calls, [True, False])
        self.assertFalse(client._cfg["web_enabled"])

    def test_password_not_stored_fails_and_never_enables(self):
        client = FakeSecClient(pw_ok=False)
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("set_web_password", result.reason)
        # set_policy(enable) must never be called when the password wasn't confirmed
        self.assertNotIn(True, client.set_policy_calls)
        # restore (to False) still attempted
        self.assertIn(False, client.set_policy_calls)

    def test_page_shell_redirect_to_login_fails(self):
        """Lazy login (2026-09-24): a page shell redirecting to /login is
        exactly the behaviour the owner rejected. FakeSecClient's
        get_status_location stands in for the observed (non-followed)
        status directly."""
        client = FakeSecClient(admin_page_status=302, admin_page_location="/login?return=/settings/zones")
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("settings/zones", result.reason)

    def test_page_shell_refused_fails(self):
        """A 401 on the shell itself (the pre-handler exemption missing)
        fails too -- only a plain 200 counts."""
        client = FakeSecClient(admin_page_status=401, admin_page_location=None)
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("settings/zones", result.reason)

    def test_api_route_not_gated_fails_even_with_page_gated(self):
        client = FakeSecClient(api_status=200)
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("api/zones", result.reason)

    def test_dashboard_probe_records_status_and_body_on_success(self):
        """2026-09-24 fix: `observed` must carry dashboard_status/body, not
        be empty, even on the happy path -- this is the evidence the live
        bench failure lacked."""
        client = FakeSecClientWithBody()
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("dashboard_status"), 200)
        self.assertIn("dashboard", result.observed.get("dashboard_body") or "")

    def test_dashboard_probe_retries_then_succeeds(self):
        """The live bug: a single-worker httpd can still be busy right after
        set_policy(web_enabled=1); the probe must retry before failing."""
        client = FakeSecClientWithBody(dashboard_bodies=[(503, "busy"), (503, "busy"), (200, "<html>ok</html>")])
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        with mock.patch.object(C, "time") as fake_time:
            result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(client.get_status_body_calls, 3)
        self.assertEqual(fake_time.sleep.call_count, 2)
        self.assertEqual(result.observed.get("dashboard_status"), 200)

    def test_dashboard_probe_exhausts_retries_and_fails_with_evidence(self):
        client = FakeSecClientWithBody(dashboard_bodies=[(503, "busy1"), (503, "busy2"), (503, "busy3")])
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        with mock.patch.object(C, "time") as fake_time:
            result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(client.get_status_body_calls, C._DASHBOARD_PROBE_ATTEMPTS)
        # observed still carries the last attempt's status/body -- never empty
        self.assertEqual(result.observed.get("dashboard_status"), 503)
        self.assertIn("busy3", result.observed.get("dashboard_body") or "")

    def test_plain_fake_client_without_get_status_body_still_works(self):
        """Backward compatibility: every pre-existing FakeSecClient-based
        test above (no get_status_body) must keep passing via the
        status-only fallback in _probe_dashboard."""
        client = FakeSecClient()
        self.assertFalse(hasattr(client, "get_status_body"))
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("dashboard_status"), 200)
        self.assertIsNone(result.observed.get("dashboard_body"))

    def test_exception_mid_flow_still_restores(self):
        """finally-discipline: a raising login() must not prevent the
        restore set_policy(False) call from running."""
        client = FakeSecClient(login_raises=True)
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        with self.assertRaises(RuntimeError):
            C._case_web_sec03(ctx)
        self.assertIn(False, client.set_policy_calls)
        self.assertFalse(client._cfg["web_enabled"])

    def test_restore_failure_fails_even_if_everything_else_passed(self):
        client = FakeSecClient(restore_ok=False)
        ctx = {"suite": "web", "sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)


def _zones_body(**over):
    """A healthy 3-zone /api/zones response, trimmed to what the zone graphic
    reads (docs/ZONE_GRAPHIC_PLAN.md section 4). Relay 4 is unowned."""
    body = {
        "generation": 7,
        "thermo_count": 3,
        "relay_zone_owned_mask": 0b0111,
        "relay_names": ["Heat1", "Heat2", "Heat3", "Vent"],
        "relay_types": [0, 0, 0, 4],
        "zones": [
            {"relay_mask": 1, "thermo_mask": 1, "ct_mask": 1, "zone_type": 0},
            {"relay_mask": 2, "thermo_mask": 2, "ct_mask": 2, "zone_type": 0},
            {"relay_mask": 4, "thermo_mask": 4, "ct_mask": 4, "zone_type": 1},
        ],
    }
    body.update(over)
    return body


class WebZone14Test(unittest.TestCase):
    def _run(self, first, second=None):
        fake = FakeHttp(
            get_queues={"/api/zones": [(200, first), (200, second if second is not None else first)]},
            post_responses={},
        )
        ctx = {"suite": "web", "http_get_json": fake.get, "http_post_json": fake.post}
        return C._case_web_zone14(ctx), fake

    def test_healthy_config_passes_and_never_posts(self):
        result, fake = self._run(_zones_body())
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(fake.post_calls, [])
        self.assertEqual(result.observed["unset_unowned_relays"], [])

    def test_unset_type_on_unowned_relay_is_reported_not_failed(self):
        result, _ = self._run(_zones_body(relay_types=[0, 0, 0, 0]))
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed["unset_unowned_relays"], [4])

    def test_owned_mask_disagreeing_with_zones_fails(self):
        result, _ = self._run(_zones_body(relay_zone_owned_mask=0b1111))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("relay_zone_owned_mask", result.reason)

    def test_thermo_count_out_of_range_fails(self):
        for bad in (-1, 4, "3", None):
            result, _ = self._run(_zones_body(thermo_count=bad))
            self.assertEqual(result.verdict, Verdict.FAIL, bad)

    def test_fewer_zones_than_thermo_count_fails(self):
        body = _zones_body()
        body["zones"] = body["zones"][:2]
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_bad_zone_field_fails_naming_it(self):
        body = _zones_body()
        body["zones"][1]["zone_type"] = 2
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("zones[1].zone_type", result.reason)

    def test_relay_type_out_of_range_or_wrong_length_fails(self):
        for bad in ([0, 0, 0, 7], [0, 0, 0], [0, 0, 0, True], "0000"):
            result, _ = self._run(_zones_body(relay_types=bad))
            self.assertEqual(result.verdict, Verdict.FAIL, bad)

    def test_missing_relay_names_fails(self):
        body = _zones_body()
        del body["relay_names"]
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_http_failure_fails(self):
        fake = FakeHttp(get_queues={"/api/zones": [(401, None)]}, post_responses={})
        result = C._case_web_zone14({"http_get_json": fake.get, "http_post_json": fake.post})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_generation_change_between_reads_is_inconclusive(self):
        result, _ = self._run(_zones_body(), _zones_body(generation=8))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_on_off_zone_with_no_thermocouple_passes_and_is_reported(self):
        body = _zones_body()
        body["zones"][2]["thermo_mask"] = 0
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["zones_without_thermocouple"], [2])

    def test_thermo_mask_bit_outside_thermo_count_fails(self):
        body = _zones_body(thermo_count=2, relay_zone_owned_mask=0b0011)
        body["zones"][0]["thermo_mask"] = 4
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("zones[0].thermo_mask", result.reason)

    def test_empty_config_is_inconclusive(self):
        result, _ = self._run(_zones_body(thermo_count=0, zones=[], relay_zone_owned_mask=0))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_negative_thermo_count_fails(self):
        result, _ = self._run(_zones_body(thermo_count=-1))
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_mask_ranges_fail(self):
        for key, bad in (("relay_mask", 16), ("relay_mask", -1), ("thermo_mask", 8),
                         ("ct_mask", 8), ("ct_mask", -1)):
            body = _zones_body()
            body["zones"][0][key] = bad
            result, _ = self._run(body)
            self.assertEqual(result.verdict, Verdict.FAIL, (key, bad))

    def test_non_dict_zone_entry_fails(self):
        body = _zones_body()
        body["zones"][1] = "oops"
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("zones[1] is not an object", result.reason)

    def test_second_read_failure_is_reported_separately(self):
        fake = FakeHttp(get_queues={"/api/zones": [(200, _zones_body()), (503, None)]}, post_responses={})
        result = C._case_web_zone14({"http_get_json": fake.get, "http_post_json": fake.post})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("second read failed (status=503)", result.reason)

    def test_missing_generation_is_noted_not_silent(self):
        body = _zones_body()
        del body["generation"]
        result, _ = self._run(body)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed["generation"], "not reported")

    def test_registered_and_in_full_suite_only(self):
        from kilnctrl.bench_test.registry import SUITES

        self.assertIsNotNone(REGISTRY["WEB-ZONE-14"].judge)
        self.assertFalse(REGISTRY["WEB-ZONE-14"].heat)
        self.assertIn("WEB-ZONE-14", SUITES["full"])


# ---------------------------------------------------------------------------
# Registry wiring
# ---------------------------------------------------------------------------

class RegistryWiringTest(unittest.TestCase):
    def test_all_wave2_web_ids_wired(self):
        for cid in ("WEB-DASH-13", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-SEC-03", "WEB-SEC-04"):
            self.assertIsNotNone(REGISTRY[cid].judge, f"{cid} has no judge wired")

    def test_ids_present_in_nightly_suite(self):
        from kilnctrl.bench_test.registry import SUITES

        for cid in ("WEB-DASH-13", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-SEC-03"):
            self.assertIn(cid, SUITES["nightly"])

    def test_web_sec04_not_in_nightly_only_full(self):
        # LCD-19's PIN producer -- registry.py's _NIGHTLY_ORDER comment
        # block deliberately excludes it; only reachable via `full`/`web`.
        from kilnctrl.bench_test.registry import SUITES

        self.assertNotIn("WEB-SEC-04", SUITES["nightly"])
        self.assertIn("WEB-SEC-04", SUITES["full"])


if __name__ == "__main__":
    unittest.main()
