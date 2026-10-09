#!/usr/bin/env python3
"""Unit tests for kilnctrl.safety_log_level_http_client -- POST/api/safety/
log_level's client (found registered with no caller anywhere during the
2026-09-20 "no client" audit), all against MOCKED urllib responses. No real
socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_safety_log_level_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import safety_log_level_http_client as sl  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class ValidationTest(unittest.TestCase):
    """NEGATIVE: out-of-range/wrong-type levels must be refused client-side,
    before any request is sent -- dashboard_settings_http.c's own handler
    validates 0..UART_LOG_LEVEL_VERBOSE (4), but a client that let a bad
    value through would just get a 400 back with no clearer diagnosis."""

    def test_negative_level_refused(self):
        with self.assertRaises(sl.SafetyLogLevelHttpError):
            sl.set_safety_log_level("host", -1)

    def test_level_above_max_refused(self):
        with self.assertRaises(sl.SafetyLogLevelHttpError):
            sl.set_safety_log_level("host", sl.SAFETY_LOG_LEVEL_MAX + 1)

    def test_non_integer_level_refused(self):
        with self.assertRaises(sl.SafetyLogLevelHttpError):
            sl.set_safety_log_level("host", 2.5)

    def test_bool_is_not_accepted_as_an_integer(self):
        """bool is a subclass of int in Python -- True/False must not slip
        through as 1/0, since that would silently accept a caller's typo."""
        with self.assertRaises(sl.SafetyLogLevelHttpError):
            sl.set_safety_log_level("host", True)

    def test_no_request_sent_when_validation_fails(self):
        with unittest.mock.patch("urllib.request.urlopen") as mock_urlopen:
            with self.assertRaises(sl.SafetyLogLevelHttpError):
                sl.set_safety_log_level("host", 99)
            mock_urlopen.assert_not_called()


class RequestShapeTest(unittest.TestCase):
    def _send(self, level):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = sl.set_safety_log_level("host", level)
        return result, sent[0]

    def test_posts_form_encoded_level(self):
        _, req = self._send(3)
        self.assertEqual(req.get_method(), "POST")
        pairs = urllib.parse.parse_qsl(req.data.decode())
        self.assertEqual(pairs, [("level", "3")])

    def test_url_targets_the_right_path(self):
        _, req = self._send(0)
        self.assertTrue(req.full_url.endswith("/api/safety/log_level"))

    def test_ok_true_response_is_returned_verbatim(self):
        result, _ = self._send(4)
        self.assertEqual(result, {"ok": True})


class SendFailedTest(unittest.TestCase):
    """dashboard_settings_http.c's handler answers {"ok":false,"error":
    "send failed"} for a real, reachable-board condition (link down) -- this
    is a normal return value, not a transport error, so it must come back
    from set_safety_log_level() rather than being raised."""

    def test_send_failed_is_returned_not_raised(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                lambda req, timeout=None: _fake_response(
                    json.dumps({"ok": False, "error": "send failed"}).encode())):
            result = sl.set_safety_log_level("host", 1)
        self.assertEqual(result, {"ok": False, "error": "send failed"})


class HttpErrorTest(unittest.TestCase):
    def test_non_2xx_detail_is_surfaced(self):
        err = urllib.error.HTTPError("u", 403, "Forbidden", {}, io.BytesIO(b"admin required"))
        with unittest.mock.patch("urllib.request.urlopen",
                                  unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(sl.SafetyLogLevelHttpError) as ctx:
                sl.set_safety_log_level("host", 1)
        self.assertIn("admin required", ctx.exception.detail)
        self.assertEqual(ctx.exception.status, 403)

    def test_unreachable_host_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(sl.SafetyLogLevelHttpError):
                sl.set_safety_log_level("host", 1)

    def test_non_json_body_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                lambda req, timeout=None: _fake_response(b"<html>nope</html>")):
            with self.assertRaises(sl.SafetyLogLevelHttpError):
                sl.set_safety_log_level("host", 1)

    # --- peer selector (e8a09c0f's per-peer log-level filter) -------------

    def test_peer_omitted_sends_no_peer_field(self):
        """The firmware owns the default (historical: "safety"). The client
        must not guess it -- an explicit peer=safety and an omitted one are
        different bodies, and only the omitted one is guaranteed to keep
        tracking the handler if that default ever moves."""
        captured = {}

        def fake(req, timeout=None):
            captured["body"] = req.data.decode("ascii")
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake):
            sl.set_safety_log_level("host", 2)
        self.assertNotIn("peer", captured["body"])
        self.assertIn("level=2", captured["body"])

    def test_peer_relay_is_sent_as_a_form_field(self):
        captured = {}

        def fake(req, timeout=None):
            captured["body"] = req.data.decode("ascii")
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake):
            sl.set_safety_log_level("host", 3, peer="relay")
        self.assertIn("peer=relay", captured["body"])

    def test_unknown_peer_is_refused_before_any_request(self):
        called = []

        def fake(req, timeout=None):
            called.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake):
            with self.assertRaises(sl.SafetyLogLevelHttpError):
                sl.set_safety_log_level("host", 1, peer="relayyyyyyy")
        self.assertEqual(called, [], "a bad peer must never reach the board")

    def test_send_failed_500_raises_and_carries_the_body(self):
        """The handler answers a link-down send failure with {"ok":false}
        under status 500, so urllib raises before the JSON is ever decoded.
        The detail must still carry the body so a caller can tell this apart
        from an unreachable board."""
        err = urllib.error.HTTPError(
            "u", 500, "Internal Server Error", {},
            io.BytesIO(b'{"ok":false,"error":"send failed"}'))
        with unittest.mock.patch("urllib.request.urlopen",
                                  unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(sl.SafetyLogLevelHttpError) as ctx:
                sl.set_safety_log_level("host", 1)
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("send failed", ctx.exception.detail)


if __name__ == "__main__":
    unittest.main()
