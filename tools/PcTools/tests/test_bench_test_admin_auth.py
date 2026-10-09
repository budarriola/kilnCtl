#!/usr/bin/env python3
"""Unit tests confirming bench_test's admin-tier HTTP helpers authenticate
through kilnctrl.http_auth's login-and-retry seam instead of failing a bare
401 -- the fix for FL-06 (`/api/coredump/info`), SK-03
(`/api/saftyfw_stack_margin`), WEB-DIAG-07/08 (`/api/watchdog_cfg`,
`/api/ramp_assist`) and WEB-SEC-03 (`/api/auth/config`) all 401ing when a
bench board already has web auth enabled.

All against a MOCKED urllib.request.urlopen -- no socket, no live board.
Same _Recorder shape as test_http_auth.py.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_admin_auth.py -q
"""
from __future__ import annotations

import email.message
import io
import os
import sys
import unittest
import unittest.mock
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import http_auth  # noqa: E402
from kilnctrl.bench_test import cases_smoke as CS  # noqa: E402
from kilnctrl.bench_test import cases_web_rw as CWR  # noqa: E402

HOST = "10.0.0.5"
ORIGIN = f"http://{HOST}"


def _response(body: bytes = b"{}", set_cookie: "str | None" = None):
    resp = io.BytesIO(body)
    resp.status = 200
    resp.getcode = lambda: 200
    headers = email.message.Message()
    if set_cookie is not None:
        headers["Set-Cookie"] = set_cookie
    resp.headers = headers

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _unauthorized(url: str):
    return urllib.error.HTTPError(url, 401, "Unauthorized", email.message.Message(), None)


class _Recorder:
    """Records each request and replays a scripted list of outcomes, same
    convention as test_http_auth.py's own recorder."""

    def __init__(self, *outcomes):
        self.outcomes = list(outcomes)
        self.requests: "list" = []

    def __call__(self, req, timeout=None):
        self.requests.append(req)
        if not self.outcomes:
            raise AssertionError("unexpected extra request")
        outcome = self.outcomes.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    @property
    def urls(self):
        return [r if isinstance(r, str) else r.full_url for r in self.requests]


class _CredsMixin:
    def setUp(self):
        http_auth.clear_sessions()
        self.addCleanup(http_auth.clear_sessions)
        patcher = unittest.mock.patch.dict(
            os.environ,
            {http_auth.USERNAME_ENV: "bench", http_auth.PASSWORD_ENV: "not-a-real-password"},
        )
        patcher.start()
        self.addCleanup(patcher.stop)


class HttpGetJsonAuthTest(_CredsMixin, unittest.TestCase):
    """cases_smoke._http_get_json -- used by FL-06, SK-03, and AT-01's
    ramp_assist precondition read."""

    def test_401_then_login_then_success(self):
        get_url = f"{ORIGIN}/api/coredump/info"
        login_url = f"{ORIGIN}{http_auth.LOGIN_PATH}"
        recorder = _Recorder(
            _unauthorized(get_url),
            _response(b"{}", set_cookie=f"{http_auth.SESSION_COOKIE_NAME}=abc123; Path=/"),
            _response(b'{"present": false}'),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            status, body = CS._http_get_json(HOST, "/api/coredump/info")
        self.assertEqual(status, 200)
        self.assertEqual(body, {"present": False})
        self.assertEqual(recorder.urls, [get_url, login_url, get_url])

    def test_credential_never_leaks_into_returned_error_text(self):
        """A login failure must surface as text with the password nowhere
        in it -- callers of _http_get_json print `body`/the reason string
        directly into CaseResult.reason, which can land in a report."""
        get_url = f"{ORIGIN}/api/saftyfw_stack_margin"
        recorder = _Recorder(
            _unauthorized(get_url),
            _unauthorized(f"{ORIGIN}{http_auth.LOGIN_PATH}"),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            status, body = CS._http_get_json(HOST, "/api/saftyfw_stack_margin")
        self.assertIsNone(status)
        self.assertNotIn("not-a-real-password", body)


class BoolToggleAdminAuthTest(_CredsMixin, unittest.TestCase):
    """cases_web_rw._get_json/_post_json (WEB-DASH-13, WEB-DIAG-07,
    WEB-DIAG-08) -- real transport, no ctx override, so the fix under test
    (http_auth.urlopen instead of a bare urllib call) is actually exercised."""

    def test_diag07_round_trip_authenticates_through_401(self):
        get_url = f"{ORIGIN}/api/watchdog_cfg"
        login_url = f"{ORIGIN}{http_auth.LOGIN_PATH}"
        post_url = f"{ORIGIN}/api/watchdog_cfg"
        recorder = _Recorder(
            # initial GET current value: 401 -> login -> retry succeeds
            _unauthorized(get_url),
            _response(b"{}", set_cookie=f"{http_auth.SESSION_COOKIE_NAME}=sess1; Path=/"),
            _response(b'{"panic_disabled": false}'),
            # POST test value (session already remembered -- no relogin)
            _response(b'{"ok": true}'),
            # GET-verify after write
            _response(b'{"panic_disabled": true}'),
            # POST restore (finally)
            _response(b'{"ok": true}'),
            # GET-verify restore
            _response(b'{"panic_disabled": false}'),
        )
        ctx = {"suite": "web", "host": HOST}
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            result = CWR._case_diag07(ctx)
        self.assertEqual(result.verdict, "PASS", result.reason)
        # First GET 401'd, then a single login, then every subsequent call
        # reused the remembered session cookie with no further logins.
        self.assertEqual(recorder.urls.count(login_url), 1)
        # 6 requests hit /api/watchdog_cfg: the initial 401'd GET, its
        # authenticated retry, POST-write, GET-verify, POST-restore,
        # GET-verify-restore -- only the ONE login above is inserted among
        # them, never a second.
        self.assertEqual(recorder.urls.count(post_url), 6)

    def test_no_credential_in_any_case_result_field(self):
        get_url = f"{ORIGIN}/api/ramp_assist"
        recorder = _Recorder(
            _unauthorized(get_url),
            _unauthorized(f"{ORIGIN}{http_auth.LOGIN_PATH}"),
        )
        ctx = {"suite": "web", "host": HOST}
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            result = CWR._case_diag08(ctx)
        self.assertEqual(result.verdict, "FAIL")
        blob = repr(result.reason) + repr(result.observed)
        self.assertNotIn("not-a-real-password", blob)


class SecClientAdminAuthTest(_CredsMixin, unittest.TestCase):
    """_SecHttpClient.get_config()/set_web_password()/set_policy() --
    WEB-SEC-03's own admin-tier calls; login()/get_session()/
    extend_session()/get_status() are intentionally untouched (they test the
    unauthenticated/explicit-cookie view) and are not covered here."""

    def test_get_config_authenticates_through_401(self):
        get_url = f"{ORIGIN}/api/auth/config"
        login_url = f"{ORIGIN}{http_auth.LOGIN_PATH}"
        recorder = _Recorder(
            _unauthorized(get_url),
            _response(b"{}", set_cookie=f"{http_auth.SESSION_COOKIE_NAME}=sess2; Path=/"),
            _response(b'{"web_enabled": true}'),
        )
        client = CWR._SecHttpClient(HOST)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            status, cfg = client.get_config()
        self.assertEqual(status, 200)
        self.assertEqual(cfg, {"web_enabled": True})
        self.assertEqual(recorder.urls, [get_url, login_url, get_url])

    def test_set_web_password_authenticates_through_401(self):
        post_url = f"{ORIGIN}/api/auth/security"
        login_url = f"{ORIGIN}{http_auth.LOGIN_PATH}"
        recorder = _Recorder(
            _unauthorized(post_url),
            _response(b"{}", set_cookie=f"{http_auth.SESSION_COOKIE_NAME}=sess3; Path=/"),
            _response(b'{"ok": true}'),
        )
        client = CWR._SecHttpClient(HOST)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            status, resp = client.set_web_password("bench", "not-a-real-password")
        self.assertEqual(status, 200)
        self.assertTrue(resp.get("ok"))
        self.assertEqual(recorder.urls, [post_url, login_url, post_url])
        # The credential this test used must not appear verbatim in any
        # request URL captured above (form body is not urlencoded into the
        # URL for a POST, but this guards the invariant explicitly anyway).
        for url in recorder.urls:
            self.assertNotIn("not-a-real-password", url)


if __name__ == "__main__":
    unittest.main()
