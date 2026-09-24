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

        def fake_urlopen(req, timeout=None):
            captured["header"] = req.get_header("Accept-encoding")
            return _FakeHttpResponse(200, b"plain body", {})

        with mock.patch.object(C.urllib.request, "urlopen", side_effect=fake_urlopen):
            status, text, _headers = client._get("/")
        self.assertEqual(captured["header"], "gzip")
        self.assertEqual(status, 200)
        self.assertEqual(text, "plain body")

    def test_decodes_gzip_content_encoding(self):
        client = C._SecHttpClient("1.2.3.4")
        payload = gzip.compress(b"<html>hello</html>")

        def fake_urlopen(req, timeout=None):
            return _FakeHttpResponse(200, payload, {"Content-Encoding": "gzip"})

        with mock.patch.object(C.urllib.request, "urlopen", side_effect=fake_urlopen):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 200)
        self.assertEqual(text, "<html>hello</html>")

    def test_malformed_gzip_body_does_not_crash_and_returns_no_body(self):
        """Advisory fix: a Content-Encoding: gzip header on a body that
        doesn't actually decompress must not raise out of _get()."""
        client = C._SecHttpClient("1.2.3.4")

        def fake_urlopen(req, timeout=None):
            return _FakeHttpResponse(200, b"not actually gzip", {"Content-Encoding": "gzip"})

        with mock.patch.object(C.urllib.request, "urlopen", side_effect=fake_urlopen):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 200)
        self.assertIsNone(text)

    def test_http_error_branch_decodes_gzip_body_and_reports_status(self):
        client = C._SecHttpClient("1.2.3.4")
        payload = gzip.compress(b'{"ok": false, "reason": "forbidden"}')
        headers = _FakeHeaders({"Content-Encoding": "gzip"})

        def fake_urlopen(req, timeout=None):
            raise urllib.error.HTTPError(
                url="http://1.2.3.4/", code=403, msg="Forbidden", hdrs=headers, fp=io.BytesIO(payload)
            )

        with mock.patch.object(C.urllib.request, "urlopen", side_effect=fake_urlopen):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 403)
        self.assertEqual(text, '{"ok": false, "reason": "forbidden"}')

    def test_http_error_branch_without_gzip_reports_plain_body(self):
        client = C._SecHttpClient("1.2.3.4")
        headers = _FakeHeaders({})

        def fake_urlopen(req, timeout=None):
            raise urllib.error.HTTPError(
                url="http://1.2.3.4/", code=401, msg="Unauthorized", hdrs=headers, fp=io.BytesIO(b"nope")
            )

        with mock.patch.object(C.urllib.request, "urlopen", side_effect=fake_urlopen):
            status, text, _headers = client._get("/")
        self.assertEqual(status, 401)
        self.assertEqual(text, "nope")


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
            pw_ok=True, enabled_ok=True, dashboard_ok=True, admin_route_gated=True,
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

    def test_admin_route_not_gated_fails(self):
        result = J.judge_web_sec03(**self._happy(admin_route_gated=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("settings/zones", result.reason)


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
        ctx = {"http_get_json": fake.get, "http_post_json": fake.post}
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
        ctx = {"http_get_json": fake.get, "http_post_json": fake.post}
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
        ctx = {"http_get_json": fake.get, "http_post_json": fake.post}
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

        ctx = {"http_get_json": _get, "http_post_json": _post}
        with self.assertRaises(RuntimeError):
            C._case_dash13(ctx)
        # restore POST attempted despite the exception
        self.assertEqual(len(calls), 2)

    def test_initial_get_failure_fails(self):
        ctx = {"http_get_json": lambda path: (500, None), "http_post_json": lambda path, fields: (200, {"ok": True})}
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
        ctx = {"http_get_json": fake.get, "http_post_json": fake.post}
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
        ctx = {"http_get_json": fake.get, "http_post_json": fake.post}
        result = C._case_diag08(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)


# ---------------------------------------------------------------------------
# WEB-SEC-03 -- case-function tests via a fake ctx["sec_client"].
# ---------------------------------------------------------------------------

class FakeSecClient:
    def __init__(self, web_enabled=False, lcd_enabled=False, web_timeout_min=30, lcd_timeout_min=30,
                 pw_ok=True, enable_ok=True, dashboard_status=200, admin_status=302,
                 login_ok=True, session_ok=True, extend_status=200,
                 login_raises=False, restore_ok=True):
        self._cfg = {
            "web_enabled": web_enabled, "lcd_enabled": lcd_enabled,
            "web_timeout_min": web_timeout_min, "lcd_timeout_min": lcd_timeout_min,
        }
        self.pw_ok = pw_ok
        self.enable_ok = enable_ok
        self.dashboard_status = dashboard_status
        self.admin_status = admin_status
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
        return self.admin_status


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
            return self.admin_status, None
        if self._bodies:
            idx = min(self.get_status_body_calls - 1, len(self._bodies) - 1)
            return self._bodies[idx]
        return self.dashboard_status, "<html>dashboard</html>"


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
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        # enabled then restored to original (False)
        self.assertEqual(client.set_policy_calls, [True, False])
        self.assertFalse(client._cfg["web_enabled"])

    def test_password_not_stored_fails_and_never_enables(self):
        client = FakeSecClient(pw_ok=False)
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("set_web_password", result.reason)
        # set_policy(enable) must never be called when the password wasn't confirmed
        self.assertNotIn(True, client.set_policy_calls)
        # restore (to False) still attempted
        self.assertIn(False, client.set_policy_calls)

    def test_admin_route_not_gated_fails(self):
        client = FakeSecClient(admin_status=200)
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("settings/zones", result.reason)

    def test_dashboard_probe_records_status_and_body_on_success(self):
        """2026-09-24 fix: `observed` must carry dashboard_status/body, not
        be empty, even on the happy path -- this is the evidence the live
        bench failure lacked."""
        client = FakeSecClientWithBody()
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("dashboard_status"), 200)
        self.assertIn("dashboard", result.observed.get("dashboard_body") or "")

    def test_dashboard_probe_retries_then_succeeds(self):
        """The live bug: a single-worker httpd can still be busy right after
        set_policy(web_enabled=1); the probe must retry before failing."""
        client = FakeSecClientWithBody(dashboard_bodies=[(503, "busy"), (503, "busy"), (200, "<html>ok</html>")])
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        with mock.patch.object(C, "time") as fake_time:
            result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(client.get_status_body_calls, 3)
        self.assertEqual(fake_time.sleep.call_count, 2)
        self.assertEqual(result.observed.get("dashboard_status"), 200)

    def test_dashboard_probe_exhausts_retries_and_fails_with_evidence(self):
        client = FakeSecClientWithBody(dashboard_bodies=[(503, "busy1"), (503, "busy2"), (503, "busy3")])
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
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
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("dashboard_status"), 200)
        self.assertIsNone(result.observed.get("dashboard_body"))

    def test_exception_mid_flow_still_restores(self):
        """finally-discipline: a raising login() must not prevent the
        restore set_policy(False) call from running."""
        client = FakeSecClient(login_raises=True)
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        with self.assertRaises(RuntimeError):
            C._case_web_sec03(ctx)
        self.assertIn(False, client.set_policy_calls)
        self.assertFalse(client._cfg["web_enabled"])

    def test_restore_failure_fails_even_if_everything_else_passed(self):
        client = FakeSecClient(restore_ok=False)
        ctx = {"sec_client": client, "web_username": "admin", "web_password": "secret"}
        result = C._case_web_sec03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)


# ---------------------------------------------------------------------------
# Registry wiring
# ---------------------------------------------------------------------------

class RegistryWiringTest(unittest.TestCase):
    def test_all_wave2_web_ids_wired(self):
        for cid in ("WEB-DASH-13", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-SEC-03"):
            self.assertIsNotNone(REGISTRY[cid].judge, f"{cid} has no judge wired")

    def test_ids_present_in_nightly_suite(self):
        from kilnctrl.bench_test.registry import SUITES

        for cid in ("WEB-DASH-13", "WEB-DIAG-07", "WEB-DIAG-08", "WEB-SEC-03"):
            self.assertIn(cid, SUITES["nightly"])


if __name__ == "__main__":
    unittest.main()
