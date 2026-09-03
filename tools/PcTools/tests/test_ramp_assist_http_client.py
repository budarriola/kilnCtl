#!/usr/bin/env python3
"""Unit tests for kilnctrl.ramp_assist_http_client -- request construction
and response parsing for GET/POST /api/ramp_assist, all against MOCKED
urllib responses. No real socket and no live board is used or required.

Run with: python -m unittest discover -s tools/PcTools/tests
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

from kilnctrl import ramp_assist_http_client as ra  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class GetEnabledTest(unittest.TestCase):
    def test_parses_true(self):
        body = json.dumps({"enabled": True}).encode()
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            enabled = ra.get_enabled("192.168.4.1")
        self.assertTrue(enabled)

    def test_parses_false(self):
        body = json.dumps({"enabled": False}).encode()
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            enabled = ra.get_enabled("192.168.4.1")
        self.assertFalse(enabled)

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ra.RampAssistHttpError):
                ra.get_enabled("192.168.4.1")

    def test_rejects_missing_enabled_field(self):
        # NEGATIVE TEST for the "response has no 'enabled' field" guard:
        # without it, a malformed/future-shaped response (e.g. the board
        # serving a different JSON shape entirely) would silently raise a
        # confusing KeyError deep inside bool(data["enabled"]) instead of
        # this client's own clear RampAssistHttpError.
        body = json.dumps({"nope": True}).encode()
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            with self.assertRaises(ra.RampAssistHttpError) as ctx:
                ra.get_enabled("192.168.4.1")
        self.assertIn("enabled", str(ctx.exception))

    def test_http_error_surfaces_status_and_detail(self):
        err = urllib.error.HTTPError("http://x/api/ramp_assist", 500, "boom",
                                      hdrs=None, fp=io.BytesIO(b"internal error"))
        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ra.RampAssistHttpError) as ctx:
                ra.get_enabled("192.168.4.1")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("internal error", ctx.exception.detail)

    def test_unreachable_host_surfaces_reason(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ra.RampAssistHttpError) as ctx:
                ra.get_enabled("192.168.4.1")
        self.assertIsNone(ctx.exception.status)
        self.assertIn("unreachable", ctx.exception.detail)


class SetEnabledTest(unittest.TestCase):
    def test_sends_enabled_one_form_field(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["data"] = req.data
            captured["headers"] = dict(req.header_items())
            captured["method"] = req.get_method()
            return _fake_response(json.dumps({"ok": True, "enabled": True}).encode())

        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = ra.set_enabled("192.168.4.1", True)
        self.assertEqual(result, {"ok": True, "enabled": True})
        self.assertEqual(captured["data"], b"enabled=1")
        self.assertEqual(captured["method"], "POST")
        self.assertEqual(captured["headers"].get("Content-type"),
                          "application/x-www-form-urlencoded")

    def test_disabled_sends_enabled_zero(self):
        # NEGATIVE-TEST-ADJACENT: without str(int(enabled)) style coercion,
        # a naive `"1" if enabled else "0"` swap (or a bug that always sends
        # "1") would make every "turn it off" call silently re-enable it --
        # exactly the silent-invalidation-of-a-tuning-run hazard this whole
        # feature exists to avoid, so this direction gets its own assertion,
        # not just the "on" case above.
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["data"] = req.data
            return _fake_response(json.dumps({"ok": True, "enabled": False}).encode())

        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = ra.set_enabled("192.168.4.1", False)
        self.assertEqual(captured["data"], b"enabled=0")
        self.assertFalse(result["enabled"])

    def test_ok_false_body_passed_through(self):
        # The board's own "applied live, NVS persist failed" shape --
        # {"ok":false,"error":"..."} -- must reach the caller intact, not be
        # coerced into a raised exception (this client has no safety opinion
        # of its own; see module docstring).
        body = json.dumps({"ok": False, "error": "ESP_ERR_NVS_NOT_INITIALIZED"}).encode()
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = ra.set_enabled("192.168.4.1", True)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "ESP_ERR_NVS_NOT_INITIALIZED")

    def test_http_error_raises_with_status(self):
        err = urllib.error.HTTPError("http://x/api/ramp_assist", 400, "bad request",
                                      hdrs=None, fp=io.BytesIO(b"missing enabled field"))
        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ra.RampAssistHttpError) as ctx:
                ra.set_enabled("192.168.4.1", True)
        self.assertEqual(ctx.exception.status, 400)
        self.assertIn("missing enabled field", ctx.exception.detail)

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ra.RampAssistHttpError):
                ra.set_enabled("192.168.4.1", True)


if __name__ == "__main__":
    unittest.main()
