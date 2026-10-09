#!/usr/bin/env python3
"""Unit tests for kilnctrl.web_auth_setup_http_client -- GET /api/auth/config,
POST /api/auth/bootstrap_password, POST /api/auth/security, and
POST /api/auth/login, all against MOCKED urllib responses. No real socket
and no live board.

Run with:
  python -m pytest tools/PcTools/tests/test_web_auth_setup_http_client.py -q
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

from kilnctrl import web_auth_setup_http_client as wac  # noqa: E402
from kilnctrl import http_auth  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class GetAuthConfigTest(unittest.TestCase):
    def test_get_via_http_auth_urlopen(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(json.dumps({"web_enabled": False, "admin_password_set": False}).encode())

        with unittest.mock.patch.object(http_auth, "urlopen", fake_urlopen):
            result = wac.get_auth_config("host")
        self.assertEqual(sent[0].get_method(), "GET")
        self.assertTrue(sent[0].full_url.endswith("/api/auth/config"))
        self.assertEqual(result, {"web_enabled": False, "admin_password_set": False})

    def test_non_json_body_is_refused(self):
        with unittest.mock.patch.object(
                http_auth, "urlopen", lambda req, timeout=None: _fake_response(b"nope")):
            with self.assertRaises(wac.WebAuthSetupHttpError):
                wac.get_auth_config("host")

    def test_unreachable_host_is_refused(self):
        with unittest.mock.patch.object(
                http_auth, "urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(wac.WebAuthSetupHttpError):
                wac.get_auth_config("host")


class BootstrapPasswordTest(unittest.TestCase):
    """POST /api/auth/bootstrap_password is a plain, unauthenticated
    request -- never routed through http_auth -- since the route requires
    no session at all (ROUTE_TIER_ADMIN_BOOTSTRAP)."""

    def test_posts_form_fields_without_http_auth(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch.object(http_auth, "urlopen") as auth_mock, \
             unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = wac.post_bootstrap_password("host", "admin", "s3cret!")
        auth_mock.assert_not_called()
        self.assertEqual(sent[0].get_method(), "POST")
        body = sent[0].data.decode()
        self.assertIn("username=admin", body)
        self.assertEqual(result, {"ok": True})

    def test_password_value_is_urlencoded_present_but_never_in_exception_text(self):
        err = urllib.error.HTTPError(
            "u", 409, "Conflict", {},
            io.BytesIO(json.dumps({"ok": False, "error": "administrator credential already configured"}).encode()))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(wac.WebAuthSetupHttpError) as ctx:
                wac.post_bootstrap_password("host", "admin", "hunter2-secret")
        self.assertEqual(ctx.exception.status, 409)
        self.assertNotIn("hunter2-secret", str(ctx.exception))
        self.assertNotIn("hunter2-secret", ctx.exception.detail)

    def test_400_weak_password_is_raised_with_status(self):
        err = urllib.error.HTTPError(
            "u", 400, "Bad Request", {}, io.BytesIO(b"password rejected"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(wac.WebAuthSetupHttpError) as ctx:
                wac.post_bootstrap_password("host", "admin", "weak")
        self.assertEqual(ctx.exception.status, 400)


class PostSecurityTest(unittest.TestCase):
    def test_posts_via_http_auth_urlopen(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch.object(http_auth, "urlopen", fake_urlopen):
            result = wac.post_security("host", {
                "cmd": "set_web_password", "role": "admin",
                "username": "admin", "password": "s3cret!",
            })
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertTrue(sent[0].full_url.endswith("/api/auth/security"))
        self.assertEqual(result, {"ok": True})

    def test_rejected_command_returns_ok_false_body(self):
        def fake_urlopen(req, timeout=None):
            return _fake_response(json.dumps({"ok": False, "error": "password too weak"}).encode())

        with unittest.mock.patch.object(http_auth, "urlopen", fake_urlopen):
            result = wac.post_security("host", {"cmd": "set_web_password"})
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "password too weak")

    def test_auth_error_from_http_auth_is_wrapped(self):
        with unittest.mock.patch.object(
                http_auth, "urlopen",
                unittest.mock.Mock(side_effect=http_auth.HttpAuthError("no credential"))):
            with self.assertRaises(wac.WebAuthSetupHttpError):
                wac.post_security("host", {"cmd": "set_policy"})


class TryLoginTest(unittest.TestCase):
    """POST /api/auth/login, bypassing http_auth's own retry machinery --
    this is the explicit, one-shot credential check web_auth_setup() uses
    when an admin record already exists."""

    def test_success_sends_identity_encoding_and_returns_true(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            ok = wac.try_login("host", "admin", "s3cret!")
        self.assertTrue(ok)
        self.assertEqual(sent[0].get_header("Accept-encoding"), "identity")
        self.assertTrue(sent[0].full_url.endswith("/api/auth/login"))
        body = sent[0].data.decode()
        self.assertIn("username=admin", body)

    def test_401_returns_false_not_raised(self):
        err = urllib.error.HTTPError("u", 401, "Unauthorized", {}, io.BytesIO(b"denied"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            ok = wac.try_login("host", "admin", "wrong-password")
        self.assertFalse(ok)

    def test_other_http_error_is_raised(self):
        err = urllib.error.HTTPError("u", 500, "Internal Server Error", {}, io.BytesIO(b"oops"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(wac.WebAuthSetupHttpError):
                wac.try_login("host", "admin", "s3cret!")

    def test_unreachable_is_raised(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(wac.WebAuthSetupHttpError):
                wac.try_login("host", "admin", "s3cret!")

    def test_password_never_in_raised_error_text(self):
        err = urllib.error.HTTPError("u", 500, "Internal Server Error", {}, io.BytesIO(b"oops"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(wac.WebAuthSetupHttpError) as ctx:
                wac.try_login("host", "admin", "very-secret-value")
        self.assertNotIn("very-secret-value", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
