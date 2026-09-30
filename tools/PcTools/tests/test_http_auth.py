#!/usr/bin/env python3
"""Unit tests for kilnctrl.http_auth -- the single seam every PcTools HTTP
client issues its requests through.

All against a MOCKED urllib.request.urlopen: no socket, no live board. The
four paths the seam has to get right are one test each -- no credential, a
401 answered by a login and one retry, a first-time success that must not
log in at all, and a login that itself fails -- plus the two boundaries that
keep it honest: a non-401 failure is never retried, and a retry is never
itself retried.

Run with: python -m pytest tools/PcTools/tests/test_http_auth.py -q
"""
from __future__ import annotations

import email.message
import io
import os
import sys
import traceback
import unittest
import unittest.mock
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import http_auth  # noqa: E402

HOST = "http://192.0.2.10"
URL = HOST + "/api/ota/pico/status"
TOKEN = "d3adb33f" * 8


def _response(body: bytes = b"{}", set_cookie: "str | None" = None):
    """A minimal stand-in for an http.client.HTTPResponse: readable, a
    .headers mapping, and usable as a context manager."""
    resp = io.BytesIO(body)
    resp.status = 200
    headers = email.message.Message()
    if set_cookie is not None:
        headers["Set-Cookie"] = set_cookie
    resp.headers = headers

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    ctx = _Ctx()
    ctx.payload = resp
    return ctx


def _unauthorized():
    return urllib.error.HTTPError(URL, 401, "Unauthorized", email.message.Message(), None)


class _Recorder:
    """Records each request urlopen is called with and replays a scripted
    list of outcomes. An exception in the script is raised; anything else is
    returned."""

    def __init__(self, *outcomes):
        self.outcomes = list(outcomes)
        self.requests: "list[urllib.request.Request]" = []
        #: Cookie header snapshotted AT CALL TIME, so a later mutation of a
        #: shared Request object cannot make a bare first attempt look
        #: authenticated after the fact.
        self.cookies: "list[str | None]" = []

    def __call__(self, req, timeout=None):
        self.requests.append(req)
        # req may be a bare URL string -- that is the pass-through case, and
        # a string carries no headers at all.
        self.cookies.append(req.get_header("Cookie") if hasattr(req, "get_header") else None)
        if not self.outcomes:
            raise AssertionError(f"unexpected extra request to {req.full_url}")
        outcome = self.outcomes.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    @property
    def urls(self):
        return [r if isinstance(r, str) else r.full_url for r in self.requests]

    def cookie(self, index: int):
        return self.cookies[index]


class HttpAuthTest(unittest.TestCase):
    def setUp(self):
        http_auth.clear_sessions()
        self.addCleanup(http_auth.clear_sessions)

    def _with_credentials(self):
        patcher = unittest.mock.patch.dict(
            os.environ,
            {http_auth.USERNAME_ENV: "admin", http_auth.PASSWORD_ENV: "not-a-real-password"},
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def _without_credentials(self):
        patcher = unittest.mock.patch.dict(os.environ, {}, clear=False)
        patcher.start()
        os.environ.pop(http_auth.USERNAME_ENV, None)
        os.environ.pop(http_auth.PASSWORD_ENV, None)
        self.addCleanup(patcher.stop)

    # --- 1. no credential ------------------------------------------------
    def test_401_without_credentials_raises_actionable_error(self):
        """A 401 with nothing in the environment must name both variables
        and must not have attempted a login."""
        self._without_credentials()
        recorder = _Recorder(_unauthorized())
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(http_auth.HttpAuthError) as caught:
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
        message = str(caught.exception)
        self.assertIn(http_auth.USERNAME_ENV, message)
        self.assertIn(http_auth.PASSWORD_ENV, message)
        self.assertEqual(recorder.urls, [URL], "no login may be attempted without a credential")

    def test_missing_credential_error_never_contains_a_password(self):
        """Only the variable NAMES may appear -- never a value."""
        with unittest.mock.patch.dict(
                os.environ, {http_auth.USERNAME_ENV: "admin", http_auth.PASSWORD_ENV: ""}):
            with self.assertRaises(http_auth.HttpAuthError) as caught:
                http_auth.credentials()
        self.assertIn(http_auth.PASSWORD_ENV, str(caught.exception))

    # --- 2. 401 -> login -> retry ---------------------------------------
    def test_401_logs_in_once_and_retries_with_the_cookie(self):
        self._with_credentials()
        recorder = _Recorder(
            _unauthorized(),
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}; HttpOnly; Path=/"),
            _response(b'{"phase":"idle"}'),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with http_auth.urlopen(urllib.request.Request(URL), timeout=2.0) as resp:
                body = resp.read()
        self.assertEqual(body, b'{"phase":"idle"}')
        self.assertEqual(recorder.urls, [URL, HOST + http_auth.LOGIN_PATH, URL])
        self.assertIsNone(recorder.cookie(0), "the first attempt must be a bare request")
        self.assertEqual(recorder.cookie(2), f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}",
                         "the retry must carry the session cookie")
        login_req = recorder.requests[1]
        self.assertEqual(login_req.get_method(), "POST")
        self.assertIn(b"username=admin", login_req.data)
        self.assertIn(b"password=", login_req.data)

    def test_session_is_reused_on_the_next_request_without_a_second_login(self):
        """Having paid for one login, a later call attaches the cookie up
        front -- one login per process, not one per request."""
        self._with_credentials()
        recorder = _Recorder(
            _unauthorized(),
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}"),
            _response(b"{}"),
            _response(b"{}"),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
            http_auth.urlopen(urllib.request.Request(HOST + "/api/zones"), timeout=2.0)
        self.assertEqual(len(recorder.urls), 4)
        self.assertEqual(recorder.cookie(3), f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}")

    def test_the_callers_request_object_is_never_mutated(self):
        """The cookie goes on a copy. A Request the caller reuses must come
        back exactly as it went in."""
        self._with_credentials()
        recorder = _Recorder(
            _unauthorized(),
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}"),
            _response(b"{}"),
        )
        caller_request = urllib.request.Request(URL)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            http_auth.urlopen(caller_request, timeout=2.0)
        self.assertIsNone(caller_request.get_header("Cookie"))

    def test_a_401_on_the_retry_is_not_retried_again(self):
        self._with_credentials()
        recorder = _Recorder(
            _unauthorized(),
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}"),
            _unauthorized(),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(urllib.error.HTTPError):
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
        self.assertEqual(len(recorder.urls), 3, "exactly one login and one retry, never a loop")

    # --- 3. success first time ------------------------------------------
    def test_success_makes_no_login_call_and_sends_no_cookie(self):
        """The auth-disabled path, which is the board's state today: one
        request, byte-for-byte what the caller built."""
        self._with_credentials()
        recorder = _Recorder(_response(b'{"phase":"idle"}'))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with http_auth.urlopen(urllib.request.Request(URL), timeout=2.0) as resp:
                self.assertEqual(resp.read(), b'{"phase":"idle"}')
        self.assertEqual(recorder.urls, [URL])
        self.assertIsNone(recorder.cookie(0))

    def test_non_401_failure_is_raised_untouched_and_never_retried(self):
        self._with_credentials()
        err = urllib.error.HTTPError(URL, 500, "Internal Server Error", email.message.Message(), None)
        recorder = _Recorder(err)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
        self.assertEqual(caught.exception.code, 500)
        self.assertEqual(recorder.urls, [URL])

    def test_a_plain_url_string_is_passed_through_unchanged(self):
        """Several callers pass a bare URL string. With nothing to attach,
        urllib must receive that same string -- not a Request wrapper. A
        test elsewhere in this suite (test_cfg_convert's export-endpoint
        assertion) reads the argument it was handed, and more to the point
        requirement: the no-auth path has to stay what it was."""
        self._with_credentials()
        recorder = _Recorder(_response(b"<html>"))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with http_auth.urlopen(URL, timeout=2.0) as resp:
                self.assertEqual(resp.read(), b"<html>")
        self.assertEqual(recorder.requests, [URL])

    def test_a_request_object_is_passed_through_by_identity(self):
        self._with_credentials()
        recorder = _Recorder(_response(b"{}"))
        caller_request = urllib.request.Request(URL)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            http_auth.urlopen(caller_request, timeout=2.0)
        self.assertIs(recorder.requests[0], caller_request)

    # --- 4. the login itself fails ---------------------------------------
    def test_login_refused_raises_http_auth_error_and_does_not_retry(self):
        self._with_credentials()
        login_refused = urllib.error.HTTPError(
            HOST + http_auth.LOGIN_PATH, 401, "Unauthorized", email.message.Message(), None)
        recorder = _Recorder(_unauthorized(), login_refused)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(http_auth.HttpAuthError) as caught:
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
        self.assertIn(http_auth.LOGIN_PATH, str(caught.exception))
        self.assertEqual(len(recorder.urls), 2, "a refused login must not be followed by a retry")

    def test_login_without_a_session_cookie_is_an_error(self):
        self._with_credentials()
        recorder = _Recorder(_unauthorized(), _response(b'{"ok":true}'))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(http_auth.HttpAuthError) as caught:
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
        self.assertIn(http_auth.SESSION_COOKIE_NAME, str(caught.exception))

    def test_unreachable_board_during_login_raises_http_auth_error(self):
        self._with_credentials()
        recorder = _Recorder(_unauthorized(), urllib.error.URLError("no route to host"))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(http_auth.HttpAuthError):
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)

    # --- 5. logout ---------------------------------------------------------
    def test_logout_with_no_remembered_session_makes_no_request(self):
        """Nothing to revoke -- returns False, and urlopen is never called."""
        recorder = _Recorder()
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            self.assertFalse(http_auth.logout(HOST))
        self.assertEqual(recorder.urls, [])

    def test_logout_posts_the_remembered_cookie_and_forgets_it(self):
        self._with_credentials()
        recorder = _Recorder(
            _unauthorized(),
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}"),
            _response(b"{}"),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)
        self.assertIn(HOST, http_auth._SESSIONS)

        logout_recorder = _Recorder(_response(b""))
        with unittest.mock.patch.object(urllib.request, "urlopen", logout_recorder):
            self.assertTrue(http_auth.logout(HOST))
        self.assertEqual(logout_recorder.urls, [HOST + http_auth.LOGOUT_PATH])
        self.assertEqual(logout_recorder.requests[0].get_method(), "POST")
        self.assertEqual(logout_recorder.cookie(0), f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}")
        self.assertNotIn(HOST, http_auth._SESSIONS, "logout must forget the session immediately")

    # --- 6. no_relogin ----------------------------------------------------
    def test_no_relogin_raises_401_without_attempting_a_login(self):
        """A caller presenting a session it did not get from this module
        (e.g. a CDP Chrome child's own cookie) must be told the truth about
        that specific session going stale, not have this module silently
        paper over it with a fresh env-credential login."""
        self._with_credentials()
        recorder = _Recorder(_unauthorized())
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(urllib.error.HTTPError) as caught:
                http_auth.urlopen(urllib.request.Request(URL), timeout=2.0, no_relogin=True)
        self.assertEqual(caught.exception.code, 401)
        self.assertEqual(len(recorder.urls), 1, "no login attempt may follow a 401 under no_relogin")
        self.assertNotIn(HOST, http_auth._SESSIONS)

    def test_no_relogin_success_is_unaffected(self):
        recorder = _Recorder(_response(b'{"ok":true}'))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with http_auth.urlopen(urllib.request.Request(URL), timeout=2.0, no_relogin=True) as resp:
                self.assertEqual(resp.read(), b'{"ok":true}')
        self.assertEqual(recorder.urls, [URL])

    def test_no_relogin_success_still_records_the_host_seen(self):
        """The only behavioural difference from the default path is meant to
        be "no session lookup, no login, no retry on 401" -- a genuine
        success must still update host_resolve the same way the default path
        does, so a caller using no_relogin doesn't silently stop being a
        candidate default host."""
        recorder = _Recorder(_response(b'{"ok":true}'))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder), \
                unittest.mock.patch.object(http_auth.host_resolve, "record_host_seen") as recorded:
            with http_auth.urlopen(urllib.request.Request(URL), timeout=2.0, no_relogin=True) as resp:
                resp.read()
        recorded.assert_called_once_with(HOST)

    def test_logout_forgets_the_session_even_if_the_post_itself_fails(self):
        """The board being unreachable, or refusing the POST, must not leave
        a stale cookie behind -- this process is done with that credential
        either way."""
        self._with_credentials()
        recorder = _Recorder(
            _unauthorized(),
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}"),
            _response(b"{}"),
        )
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            http_auth.urlopen(urllib.request.Request(URL), timeout=2.0)

        logout_recorder = _Recorder(urllib.error.URLError("no route to host"))
        with unittest.mock.patch.object(urllib.request, "urlopen", logout_recorder):
            self.assertTrue(http_auth.logout(HOST))
        self.assertNotIn(HOST, http_auth._SESSIONS)


class LoginTest(unittest.TestCase):
    """Direct coverage for the public http_auth.login() wrapper -- used by
    web_commission_row.py's CDP path, which needs the raw cookie VALUE
    rather than an authenticated response. Must return only the cookie,
    never a credential, and must raise on any non-2xx."""

    def setUp(self):
        http_auth.clear_sessions()
        self.addCleanup(http_auth.clear_sessions)

    def _with_credentials(self):
        patcher = unittest.mock.patch.dict(
            os.environ,
            {http_auth.USERNAME_ENV: "admin", http_auth.PASSWORD_ENV: "not-a-real-password"},
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_login_returns_only_the_cookie_value(self):
        self._with_credentials()
        recorder = _Recorder(
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}; HttpOnly; Path=/"))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            result = http_auth.login("http://192.0.2.10")
        self.assertEqual(result, TOKEN)
        self.assertEqual(recorder.urls, ["http://192.0.2.10" + http_auth.LOGIN_PATH])
        login_req = recorder.requests[0]
        self.assertEqual(login_req.get_method(), "POST")
        self.assertIn(b"username=admin", login_req.data)
        # The credential DOES go out on the wire (the board has to see it to
        # authenticate) -- what must never happen is the credential coming
        # BACK in the return value.
        self.assertIn(b"password=", login_req.data)

    def test_login_remembers_the_session_for_a_later_urlopen(self):
        self._with_credentials()
        recorder = _Recorder(
            _response(b'{"ok":true}', set_cookie=f"{http_auth.SESSION_COOKIE_NAME}={TOKEN}"))
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            http_auth.login("http://192.0.2.10")
        self.assertEqual(http_auth._SESSIONS.get("http://192.0.2.10"), TOKEN)

    def test_login_raises_http_auth_error_on_non_2xx(self):
        self._with_credentials()
        refused = urllib.error.HTTPError(
            "http://192.0.2.10" + http_auth.LOGIN_PATH, 401, "Unauthorized",
            email.message.Message(), None)
        recorder = _Recorder(refused)
        with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
            with self.assertRaises(http_auth.HttpAuthError) as caught:
                http_auth.login("http://192.0.2.10")
        self.assertNotIn("not-a-real-password", str(caught.exception))

    def test_login_generic_failure_does_not_leak_the_password(self):
        """The login POST's ``except Exception`` branch (a non-HTTPError
        failure, e.g. a connection reset) formats the exception into the
        raised HttpAuthError's message. This pins the stronger property that
        the branch scrubs the credential even when the underlying
        exception's OWN text echoes it back verbatim (e.g. a proxy or HTTP
        library that includes the request body in its error): the injected
        exception's message here deliberately DOES contain the fake
        password, and the assertion below still requires it to be absent
        from the raised error."""
        self._with_credentials()
        fake_password = "sw0rdfish-not-a-real-password"
        with unittest.mock.patch.dict(os.environ, {http_auth.PASSWORD_ENV: fake_password}):
            failure = OSError(f"connection reset by peer while sending {fake_password}")
            recorder = _Recorder(failure)
            with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
                with self.assertRaises(http_auth.HttpAuthError) as caught:
                    http_auth.login("http://192.0.2.10")
        self.assertNotIn(fake_password, str(caught.exception))
        # And the chained original must not carry it back in via a
        # traceback print: ``from None`` suppresses the context.
        self.assertIsNone(caught.exception.__cause__)
        rendered = "".join(traceback.format_exception(
            type(caught.exception), caught.exception, caught.exception.__traceback__))
        self.assertNotIn(fake_password, rendered)

    def test_login_generic_failure_scrubs_the_url_encoded_password(self):
        """The login body is form-encoded, so an underlying error that echoes
        it back carries the ``quote_plus`` form of the password, not the raw
        one. The fake password here contains characters that encoding
        changes, so the raw-form replacement alone cannot pass this."""
        self._with_credentials()
        fake_password = "p @ss+w/ord-not-real"
        encoded = urllib.parse.quote_plus(fake_password)
        self.assertNotEqual(encoded, fake_password)
        with unittest.mock.patch.dict(os.environ, {http_auth.PASSWORD_ENV: fake_password}):
            failure = OSError(f"proxy rejected body username=admin&password={encoded}")
            recorder = _Recorder(failure)
            with unittest.mock.patch.object(urllib.request, "urlopen", recorder):
                with self.assertRaises(http_auth.HttpAuthError) as caught:
                    http_auth.login("http://192.0.2.10")
        self.assertNotIn(encoded, str(caught.exception))
        self.assertNotIn(fake_password, str(caught.exception))

    def test_redact_credentials_tolerates_empty_and_none_secrets(self):
        """An empty needle in ``str.replace`` splices the replacement between
        every character, and a ``None`` needle raises -- neither may happen."""
        text = "connection reset by peer"
        for username, password in (("", ""), (None, None), ("admin", ""), ("", None)):
            with self.subTest(username=username, password=password):
                self.assertEqual(
                    http_auth._redact_credentials(text, username, password), text)
        self.assertEqual(
            http_auth._redact_credentials("peer admin reset", "admin", ""),
            "peer <redacted> reset")

    def test_login_raises_without_a_credential(self):
        with unittest.mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(http_auth.USERNAME_ENV, None)
            os.environ.pop(http_auth.PASSWORD_ENV, None)
            with self.assertRaises(http_auth.HttpAuthError):
                http_auth.login("http://192.0.2.10")


class ClientsUseTheSeamTest(unittest.TestCase):
    """The seam is only worth having if the clients actually go through it.
    This is the check that a future client added with a bare
    urllib.request.urlopen call gets caught."""

    # Narrow, named exemptions only -- (filename, function name) pairs whose
    # bench-test cases must prove a request is REFUSED (a 401/403, or "no
    # credential presented at all") without ever going through http_auth.
    # http_auth.urlopen() auto-logs-in and retries once on any 401 using
    # KILNCTL_WEB_USERNAME/PASSWORD from the environment -- exactly wrong for
    # these cases, since it would silently authenticate a request that is
    # supposed to prove it was NOT authenticated, defeating the bench-test
    # case outright. See 74f260a1 and the OT-E09/OT-E10 bench-test cases.
    # A new direct urlopen call anywhere else -- or a NEW function that isn't
    # added here -- must still fail the test below.
    _EXEMPT_FUNCTIONS = {
        ("ota_http_client.py", "push_esp_image_unauthenticated"),  # OT-E09: no credential at all
        ("ota_http_client.py", "push_esp_image_with_session"),     # OT-E10: explicit session cookie, no auto-login
        # ROUTE_TIER_ADMIN_BOOTSTRAP requires no session at all (and there is
        # no admin credential yet to log in with), so this is deliberately
        # unauthenticated.
        ("web_auth_setup_http_client.py", "post_bootstrap_password"),
        # A one-shot credential check: http_auth's auto-login-on-401 would
        # defeat the "log in exactly once, never retry" contract here.
        ("web_auth_setup_http_client.py", "try_login"),
        # POST /api/auth/forgot and /api/auth/reset are ROUTE_TIER_OPEN and
        # exist for an operator who has LOST the password: auto-login-on-401
        # with KILNCTL_WEB_PASSWORD (or attaching a remembered session
        # cookie) has no place in that flow (docs/TOTP_PASSWORD_RESET_PLAN.md
        # section 4; totp_http_client.py's route-tier note).
        ("totp_http_client.py", "_post_open"),
        # firmware/KilnFW_recovery/'s own, untouched AP-password X-Ota-Mac
        # scheme (recovery_ota_auth_client.py's module docstring) can only
        # ever be reached while the board is stuck in recovery mode, unable
        # to answer the main app's admin web session at all -- http_auth's
        # auto-login-on-401 has no admin session to attach here by design.
        ("recovery_ota_auth_client.py", "get_challenge"),
        ("recovery_ota_auth_client.py", "signed_post"),
    }

    @staticmethod
    def _enclosing_function(lines, lineno_0based):
        """Walk backwards from a 0-based line index to the nearest top-level
        (unindented) `def`, i.e. the function the given line lives in."""
        for i in range(lineno_0based, -1, -1):
            stripped = lines[i]
            if stripped.startswith("def ") or stripped.startswith("async def "):
                after = stripped.split("def ", 1)[1]
                return after.split("(", 1)[0].strip()
        return None

    def test_no_client_calls_urllib_urlopen_directly(self):
        pkg = os.path.join(os.path.dirname(__file__), "..", "src", "kilnctrl")
        offenders = []
        for name in sorted(os.listdir(pkg)):
            if not name.endswith(".py") or name == "http_auth.py":
                continue
            with open(os.path.join(pkg, name), encoding="utf-8") as handle:
                lines = handle.readlines()
            for lineno, line in enumerate(lines, 1):
                if "urllib.request.urlopen(" not in line:
                    continue
                func = self._enclosing_function(lines, lineno - 1)
                if (name, func) in self._EXEMPT_FUNCTIONS:
                    continue
                offenders.append(f"{name}:{lineno} (in {func})")
        self.assertEqual(
            offenders, [],
            "these call urllib.request.urlopen directly and would break the moment web "
            "authentication is enabled -- route them through http_auth.urlopen instead, or if "
            "the call is deliberately raw (proving a request is refused, where http_auth's "
            "auto-login-on-401 would defeat the case), add it to _EXEMPT_FUNCTIONS by name "
            "with a comment explaining why")

    def test_exempt_functions_still_exist_and_still_call_urlopen_directly(self):
        """Guards against the exemption list going stale (e.g. the function
        renamed or its raw call removed), which would silently exempt
        nothing -- or worse, exempt a since-repurposed function by accident
        if a new function happened to reuse the same name."""
        pkg = os.path.join(os.path.dirname(__file__), "..", "src", "kilnctrl")
        found = set()
        for name, func in self._EXEMPT_FUNCTIONS:
            path = os.path.join(pkg, name)
            with open(path, encoding="utf-8") as handle:
                lines = handle.readlines()
            in_func = False
            for line in lines:
                if line.startswith(f"def {func}(") or line.startswith(f"async def {func}("):
                    in_func = True
                    continue
                if in_func and (line.startswith("def ") or line.startswith("async def ")):
                    in_func = False
                if in_func and "urllib.request.urlopen(" in line:
                    found.add((name, func))
        missing = self._EXEMPT_FUNCTIONS - found
        self.assertEqual(
            missing, set(),
            "an exempted (file, function) no longer contains a direct urlopen call -- "
            "remove it from _EXEMPT_FUNCTIONS")


if __name__ == "__main__":
    unittest.main()
