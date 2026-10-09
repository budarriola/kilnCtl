#!/usr/bin/env python3
"""Unit tests for kilnctrl.zones_current_sweep_http_client -- request
construction and response parsing for the three current-sweep endpoints,
all against MOCKED urllib responses. No real socket and no live board is
used or required, and no relay is ever energized by this file.

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

from kilnctrl import zones_current_sweep_http_client as sw  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class StartTest(unittest.TestCase):
    def test_accepted(self):
        body = json.dumps({"ok": True, "reason": "ok"}).encode()
        with unittest.mock.patch.object(sw.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = sw.start("192.168.4.1")
        self.assertTrue(result["ok"])
        self.assertEqual(result["reason"], "ok")

    def test_refusal_409_is_not_raised_but_returned(self):
        # NEGATIVE-TESTABLE CONTRACT: a well-formed 409 refusal is a normal
        # answer, not a transport failure -- this must come back as a dict,
        # never raise. Mutating this to "raise on any non-2xx" would make
        # this test fail with a ZoneSweepHttpError instead of returning.
        err_body = json.dumps({"ok": False, "reason": "the safety link is down"}).encode()
        err = urllib.error.HTTPError("http://x/api/zones/current_sweep/start", 409, "Conflict",
                                      hdrs=None, fp=io.BytesIO(err_body))
        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=err):
            result = sw.start("192.168.4.1")
        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "the safety link is down")

    def test_malformed_409_body_raises(self):
        err = urllib.error.HTTPError("http://x/api/zones/current_sweep/start", 409, "Conflict",
                                      hdrs=None, fp=io.BytesIO(b"not json at all"))
        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(sw.ZoneSweepHttpError):
                sw.start("192.168.4.1")

    def test_rejects_response_with_no_ok_field(self):
        body = json.dumps({"nope": True}).encode()
        with unittest.mock.patch.object(sw.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            with self.assertRaises(sw.ZoneSweepHttpError) as ctx:
                sw.start("192.168.4.1")
        self.assertIn("ok", str(ctx.exception))

    def test_unreachable_host_surfaces_reason(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(sw.ZoneSweepHttpError) as ctx:
                sw.start("192.168.4.1")
        self.assertIn("unreachable", ctx.exception.detail)

    def test_posts_to_start_path(self):
        body = json.dumps({"ok": True, "reason": "ok"}).encode()
        captured = {}

        def _capture(req, timeout=None):
            captured["url"] = req.full_url
            captured["method"] = req.get_method()
            return _fake_response(body)

        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=_capture):
            sw.start("192.168.4.1")
        self.assertEqual(captured["method"], "POST")
        self.assertIn("/api/zones/current_sweep/start", captured["url"])


class AbortTest(unittest.TestCase):
    def test_ok_true(self):
        body = json.dumps({"ok": True}).encode()
        with unittest.mock.patch.object(sw.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = sw.abort("192.168.4.1")
        self.assertTrue(result["ok"])

    def test_posts_to_abort_path(self):
        body = json.dumps({"ok": True}).encode()
        captured = {}

        def _capture(req, timeout=None):
            captured["url"] = req.full_url
            captured["method"] = req.get_method()
            return _fake_response(body)

        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=_capture):
            sw.abort("192.168.4.1")
        self.assertEqual(captured["method"], "POST")
        self.assertIn("/api/zones/current_sweep/abort", captured["url"])

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(sw.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(sw.ZoneSweepHttpError):
                sw.abort("192.168.4.1")


class StatusTest(unittest.TestCase):
    def test_parses_full_shape(self):
        payload = {
            "state": "done", "zone_index": 2, "zones_done": 3, "zones_total": 3,
            "reason": "done", "ct_map_derived_mask": 0, "ct_map_reason": "n/a",
            "k_ct_derived_mask": 0, "k_ct_reason": "n/a",
            "i_normal_pushed_mask": 0, "i_normal_reason": "below noise floor",
            "summed_unmeasured_mask": 7,
            "nameplate_mismatch_mask": 0, "nameplate_reason": "n/a",
        }
        body = json.dumps(payload).encode()
        with unittest.mock.patch.object(sw.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = sw.status("192.168.4.1")
        self.assertEqual(result["state"], "done")
        self.assertEqual(result["summed_unmeasured_mask"], 7)

    def test_gets_status_path(self):
        payload = {"state": "idle"}
        body = json.dumps(payload).encode()
        captured = {}

        def _capture(req, timeout=None):
            captured["url"] = req.full_url
            captured["method"] = req.get_method()
            return _fake_response(body)

        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=_capture):
            sw.status("192.168.4.1")
        self.assertEqual(captured["method"], "GET")
        self.assertIn("/api/zones/current_sweep/status", captured["url"])

    def test_rejects_response_with_no_state_field(self):
        body = json.dumps({"nope": True}).encode()
        with unittest.mock.patch.object(sw.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            with self.assertRaises(sw.ZoneSweepHttpError) as ctx:
                sw.status("192.168.4.1")
        self.assertIn("state", str(ctx.exception))

    def test_http_error_surfaces_status_and_detail(self):
        err = urllib.error.HTTPError("http://x/api/zones/current_sweep/status", 500, "boom",
                                      hdrs=None, fp=io.BytesIO(b"internal error"))
        with unittest.mock.patch.object(sw.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(sw.ZoneSweepHttpError) as ctx:
                sw.status("192.168.4.1")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("internal error", ctx.exception.detail)


if __name__ == "__main__":
    unittest.main()
