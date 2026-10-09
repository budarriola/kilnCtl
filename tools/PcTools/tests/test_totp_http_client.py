#!/usr/bin/env python3
"""Unit tests for kilnctrl.totp_http_client -- the /api/auth/forgot,
/api/auth/reset and /api/auth/totp_status clients, all against MOCKED
urllib responses. No real socket, no live board (WT-A's firmware routes do
not exist yet -- this exercises the contract this client assumes).

Run with: python -m pytest tools/PcTools/tests/test_totp_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import totp_http_client as thc  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


#: Verbatim body of the shared login ladder's 429 (web_auth_login_http.c).
_LADDER_429_BODY = b"too many failed attempts, try again later"


class ForgotTest(unittest.TestCase):
    def test_posts_username_and_code_no_auth_seam(self):
        """This is an OPEN-tier route -- it must use plain urllib, never
        http_auth.urlopen()'s login-on-401 seam."""
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(json.dumps({"reset_token": "abc123"}).encode(), status=202)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = thc.post_forgot("host", "bench", "123456")
        self.assertEqual(status, 202)
        self.assertEqual(body, {"reset_token": "abc123"})
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertTrue(sent[0].full_url.endswith("/api/auth/forgot"))
        sent_body = sent[0].data.decode()
        self.assertIn("username=bench", sent_body)
        self.assertIn("code=123456", sent_body)

    def test_always_202_shape_survives_a_wrong_code_response(self):
        """Per the anti-oracle contract, a wrong code still comes back 202
        with a token-shaped body -- this client must not treat that as an
        error."""
        def fake_urlopen(req, timeout=None):
            return _fake_response(json.dumps({"reset_token": "dummytoken"}).encode(), status=202)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = thc.post_forgot("host", "bench", "000000")
        self.assertEqual(status, 202)
        self.assertIn("reset_token", body)

    def test_rate_limited_429_is_returned_not_raised(self):
        # The shared login ladder's real 429 body is PLAIN TEXT
        # (web_auth_login_http.c's login 429 site) -- it must still come
        # back as a status the caller can branch on, not a parse error.
        err = urllib.error.HTTPError("u", 429, "Too Many Requests", {},
                                     io.BytesIO(_LADDER_429_BODY))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = thc.post_forgot("host", "bench", "123456")
        self.assertEqual(status, 429)
        self.assertEqual(body, {})

    def test_unsynced_clock_503_plain_text_is_returned_not_raised(self):
        err = urllib.error.HTTPError("u", 503, "Service Unavailable", {},
                                     io.BytesIO(b"board clock not synced yet"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = thc.post_forgot("host", "bench", "123456")
        self.assertEqual(status, 503)
        self.assertEqual(body, {})

    def test_read_timeout_raises_totp_error_not_bare_oserror(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  unittest.mock.Mock(side_effect=TimeoutError("timed out"))):
            with self.assertRaises(thc.TotpHttpError):
                thc.post_forgot("host", "bench", "123456")

    def test_unreachable_host_raises(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  unittest.mock.Mock(side_effect=urllib.error.URLError("no route"))):
            with self.assertRaises(thc.TotpHttpError):
                thc.post_forgot("host", "bench", "123456")

    def test_non_json_body_raises(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"<html>", status=202)):
            with self.assertRaises(thc.TotpHttpError):
                thc.post_forgot("host", "bench", "123456")

    def test_code_never_appears_in_a_repr_of_the_request_error(self):
        """Defence in depth: even an exception path must not embed the
        submitted code in a way a caller might print."""
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"not json", status=202)):
            with self.assertRaises(thc.TotpHttpError) as ctx:
                thc.post_forgot("host", "bench", "999999")
        self.assertNotIn("999999", str(ctx.exception))

    def test_plain_text_400_is_returned_not_raised(self):
        err = urllib.error.HTTPError("u", 400, "Bad Request", {},
                                     io.BytesIO(b"body missing or too large"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = thc.post_forgot("host", "bench", "999999")
        self.assertEqual((status, body), (400, {}))


class ResetTest(unittest.TestCase):
    def test_success_returns_200_and_ok_true(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(json.dumps({"ok": True}).encode(), status=200)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = thc.post_reset("host", "bench", "realtoken", "N3wPassw0rd!")
        self.assertEqual(status, 200)
        self.assertEqual(body, {"ok": True})
        sent_body = sent[0].data.decode()
        self.assertIn("reset_token=realtoken", sent_body)
        self.assertIn("new_password=", sent_body)

    def test_generic_400_on_any_failure(self):
        err = urllib.error.HTTPError("u", 400, "Bad Request", {}, io.BytesIO(b'{"ok":false}'))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = thc.post_reset("host", "bench", "badtoken", "whatever")
        self.assertEqual(status, 400)
        self.assertEqual(body, {"ok": False})

    def test_rate_limited_429_is_returned_not_raised(self):
        err = urllib.error.HTTPError("u", 429, "Too Many Requests", {},
                                     io.BytesIO(_LADDER_429_BODY))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = thc.post_reset("host", "bench", "sometoken", "whatever")
        self.assertEqual((status, body), (429, {}))

    def test_password_never_appears_in_exception_text(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"not json", status=200)):
            with self.assertRaises(thc.TotpHttpError) as ctx:
                thc.post_reset("host", "bench", "tok", "SuperSecretPW123")
        self.assertNotIn("SuperSecretPW123", str(ctx.exception))


class TotpStatusTest(unittest.TestCase):
    def test_uses_admin_session_seam(self):
        """Unlike forgot/reset, this route is ADMIN-tier -- it must go
        through http_auth.urlopen(), not plain urllib."""
        sent = []

        def fake_urlopen(req, timeout=None, no_relogin=False):
            sent.append(req)
            return _fake_response(json.dumps({"enrolled": True}).encode())

        with unittest.mock.patch.object(thc.http_auth, "urlopen", fake_urlopen):
            data = thc.get_totp_status("host")
        self.assertEqual(data, {"enrolled": True})
        self.assertTrue(sent[0].full_url.endswith("/api/auth/totp_status"))
        self.assertEqual(sent[0].get_method(), "GET")

    def test_http_error_raises_with_status(self):
        err = urllib.error.HTTPError("u", 401, "Unauthorized", {}, io.BytesIO(b""))
        with unittest.mock.patch.object(thc.http_auth, "urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(thc.TotpHttpError) as ctx:
                thc.get_totp_status("host")
        self.assertEqual(ctx.exception.status, 401)


if __name__ == "__main__":
    unittest.main()
