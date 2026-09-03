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


def _no_such_endpoint_http_error(status: int = 404):
    body = json.dumps({"ok": False, "error": "no such endpoint"}).encode()
    return urllib.error.HTTPError("http://x/api/ramp_assist", status, "not found",
                                   hdrs=None, fp=io.BytesIO(body))


class PinEnabledTest(unittest.TestCase):
    """pin_enabled() -- the tolerant wrapper run_queue.py's preset-apply
    path uses. Board firmware that predates /api/ramp_assist answers with
    {"ok":false,"error":"no such endpoint"}; everything here is about
    detecting exactly that shape and nothing looser."""

    def test_false_pin_endpoint_absent_is_satisfied_not_raised(self):
        # Pinning OFF against firmware that has never heard of the feature:
        # already true by construction, so this must NOT raise, and must
        # return None (nothing was actually written) rather than a fake
        # {"ok": True} the caller might mistake for a real ack.
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         side_effect=_no_such_endpoint_http_error()):
            result = ra.pin_enabled("192.168.4.1", False)
        self.assertIsNone(result)

    def test_true_pin_endpoint_absent_raises_reflash_error(self):
        # Pinning ON against firmware that cannot provide it: this is the
        # silent-invalidation hazard, must hard-fail, and the message must
        # name the reflash requirement so an operator isn't left guessing.
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         side_effect=_no_such_endpoint_http_error()):
            with self.assertRaises(ra.RampAssistEndpointAbsentError) as ctx:
                ra.pin_enabled("192.168.4.1", True)
        self.assertIn("reflash", str(ctx.exception).lower())
        self.assertIsInstance(ctx.exception, ra.RampAssistHttpError)

    def test_endpoint_present_true_unchanged_behaviour(self):
        body = json.dumps({"ok": True, "enabled": True}).encode()
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = ra.pin_enabled("192.168.4.1", True)
        self.assertEqual(result, {"ok": True, "enabled": True})

    def test_endpoint_present_false_unchanged_behaviour(self):
        body = json.dumps({"ok": True, "enabled": False}).encode()
        with unittest.mock.patch.object(ra.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = ra.pin_enabled("192.168.4.1", False)
        self.assertEqual(result, {"ok": True, "enabled": False})

    def test_genuine_transport_error_not_swallowed(self):
        # A real HTTP 500 must NOT be mistaken for the "no such endpoint"
        # shape just because it is also a non-2xx failure -- the detection
        # must match the response body, not merely "some 4xx/5xx happened".
        err = urllib.error.HTTPError("http://x/api/ramp_assist", 500, "boom",
                                      hdrs=None, fp=io.BytesIO(b"internal error"))
        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ra.RampAssistHttpError) as ctx:
                ra.pin_enabled("192.168.4.1", False)
        self.assertNotIsInstance(ctx.exception, ra.RampAssistEndpointAbsentError)
        self.assertEqual(ctx.exception.status, 500)

    def test_timeout_not_swallowed(self):
        err = urllib.error.URLError("timed out")
        with unittest.mock.patch.object(ra.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ra.RampAssistHttpError) as ctx:
                ra.pin_enabled("192.168.4.1", True)
        self.assertNotIsInstance(ctx.exception, ra.RampAssistEndpointAbsentError)


if __name__ == "__main__":
    unittest.main()
