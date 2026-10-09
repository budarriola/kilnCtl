#!/usr/bin/env python3
"""Unit tests for mcp_server_web_auth.web_auth_logout() -- the MCP tool that
ends this process's own remembered admin session via http_auth.logout().
All against mocked http_auth/urllib calls; no real socket, no live board.

Run with:
  python -m pytest tools/PcTools/tests/test_web_auth_logout.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import http_auth  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_web_auth as msw  # noqa: E402


class WebAuthLogoutTest(unittest.TestCase):
    def setUp(self):
        # Never let a real remembered session from another test leak in, and
        # never let this test's own remembered session leak out to another.
        http_auth.clear_sessions()
        self._resolve_patch = unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")
        self._resolve_patch.start()

    def tearDown(self):
        self._resolve_patch.stop()
        http_auth.clear_sessions()

    def test_no_remembered_session_returns_false_and_sends_no_request(self):
        with unittest.mock.patch("urllib.request.urlopen") as urlopen_mock:
            result = msw.web_auth_logout()
        self.assertIs(result, False)
        urlopen_mock.assert_not_called()

    def test_remembered_session_posts_logout_and_returns_true(self):
        http_auth._SESSIONS["http://10.0.0.5"] = "abc123"
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            import io
            return io.BytesIO(b"")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = msw.web_auth_logout()
        self.assertIs(result, True)
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertTrue(sent[0].full_url.endswith("/api/auth/logout"))
        self.assertEqual(sent[0].get_header("Cookie"), "kiln_sid=abc123")
        # The session is forgotten regardless of the board's response.
        self.assertNotIn("http://10.0.0.5", http_auth._SESSIONS)

    def test_board_refusal_still_returns_true_and_forgets_session(self):
        import urllib.error
        http_auth._SESSIONS["http://10.0.0.5"] = "def456"
        err = urllib.error.HTTPError("u", 500, "Internal Server Error", {}, None)
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            result = msw.web_auth_logout()
        self.assertIs(result, True)
        self.assertNotIn("http://10.0.0.5", http_auth._SESSIONS)

    def test_explicit_host_is_resolved_and_used(self):
        with unittest.mock.patch.object(
                mcp_server_ota, "_ota_resolve_host", return_value="192.168.1.156") as resolve_mock:
            result = msw.web_auth_logout(host="192.168.1.156")
        resolve_mock.assert_called_once_with("192.168.1.156")
        self.assertIs(result, False)


if __name__ == "__main__":
    unittest.main()
