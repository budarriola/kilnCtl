#!/usr/bin/env python3
"""Unit tests for kilnctrl.recovery_post_client -- the unauthenticated POSTs
against the standalone recovery image (owner decision 2026-10-02: no password,
no key, the LCD-passphrase SoftAP is the only access control). All against
MOCKED urllib responses -- no real socket and no live board is used or required.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import http.client
import io
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import recovery_post_client as rec  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _capture(reply: bytes, status: int = 200):
    captured = {}

    def fake_urlopen(req, timeout=None):
        captured["url"] = req.full_url
        captured["data"] = req.data
        captured["method"] = req.get_method()
        captured["headers"] = {k.lower(): v for k, v in req.header_items()}
        return _fake_response(reply, status)

    return captured, unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen)


class PostTest(unittest.TestCase):
    def _assert_unauthenticated(self, captured):
        for h in captured["headers"]:
            self.assertNotIn("mac", h)
            self.assertNotIn("auth", h)
            self.assertNotIn("x-ota", h)

    def test_boot_guard_reset(self):
        captured, patch = _capture(b"boot_guard counter cleared")
        with patch:
            result = rec.recovery_boot_guard_reset("10.0.0.5")
        self.assertEqual(result, {"status": 200, "text": "boot_guard counter cleared"})
        self.assertEqual(captured["url"], "http://10.0.0.5/api/ota/esp/boot_guard_reset")
        self.assertEqual(captured["method"], "POST")
        self._assert_unauthenticated(captured)

    def test_sw_reset(self):
        captured, patch = _capture(b"resetting")
        with patch:
            result = rec.recovery_sw_reset("10.0.0.5")
        self.assertEqual(result["text"], "resetting")
        self.assertEqual(captured["url"], "http://10.0.0.5/api/sw_reset")
        self._assert_unauthenticated(captured)

    def test_pico_abort(self):
        captured, patch = _capture(b"abort requested")
        with patch:
            result = rec.recovery_pico_abort("10.0.0.5")
        self.assertEqual(result["text"], "abort requested")
        self.assertEqual(captured["url"], "http://10.0.0.5/api/recovery/pico/abort")

    def test_push_esp_image_sends_body(self):
        captured, patch = _capture(b"ok, rebooting into new application image")
        with patch:
            result = rec.recovery_push_esp_image("10.0.0.5", b"fake-image-bytes")
        self.assertEqual(result["status"], 200)
        self.assertEqual(captured["url"], "http://10.0.0.5/api/ota/esp")
        self.assertEqual(captured["data"], b"fake-image-bytes")
        self._assert_unauthenticated(captured)

    def test_apply_staged_path_and_empty_body(self):
        captured, patch = _capture(b'{"started":true,"image_length":5}', 202)
        with patch:
            result = rec.recovery_apply_staged("10.0.0.5")
        self.assertEqual(result["status"], 202)
        self.assertEqual(captured["url"], "http://10.0.0.5/api/recovery/apply_staged")
        self.assertEqual(captured["method"], "POST")
        self.assertNotIn("?", captured["url"])
        self._assert_unauthenticated(captured)

    def test_exit_and_wifi_reset_paths(self):
        for fn, path in ((rec.recovery_exit, "/api/recovery/exit"),
                         (rec.recovery_wifi_reset, "/api/recovery/wifi_reset")):
            captured, patch = _capture(b"ok")
            with patch:
                fn("10.0.0.5")
            self.assertEqual(captured["url"], "http://10.0.0.5" + path)
            self.assertNotIn("?", captured["url"])

    def test_pico_upload_url_and_body(self):
        captured, patch = _capture(b'{"started":true,"image_slot":"A"}', 202)
        with patch:
            r = rec.recovery_pico_upload("10.0.0.5", b"IMG", 0xDEADBEEF, slot="A")
        self.assertEqual(r["status"], 202)
        self.assertEqual(captured["url"], "http://10.0.0.5/api/recovery/pico/upload?crc=deadbeef&slot=A")
        self.assertEqual(captured["data"], b"IMG")
        self._assert_unauthenticated(captured)

    def test_no_challenge_is_ever_requested(self):
        # One urlopen call per POST: the retired GET /api/ota/challenge round
        # trip must not come back.
        with unittest.mock.patch("urllib.request.urlopen",
                                 side_effect=[_fake_response(b"resetting")]) as m:
            rec.recovery_sw_reset("10.0.0.5")
        self.assertEqual(m.call_count, 1)
        self.assertFalse(hasattr(rec, "get_challenge"))
        self.assertFalse(hasattr(rec, "derive_mac"))

    def test_refusal_wrapped(self):
        err = urllib.error.HTTPError("url", 409, "conflict", {}, io.BytesIO(b"pico busy"))
        with unittest.mock.patch("urllib.request.urlopen", side_effect=err):
            with self.assertRaises(rec.RecoveryPostError) as ctx:
                rec.recovery_boot_guard_reset("10.0.0.5")
        self.assertEqual(ctx.exception.status, 409)
        self.assertEqual(ctx.exception.stage, "post")
        self.assertEqual(ctx.exception.detail, "pico busy")

    def test_raw_oserror_is_wrapped_as_lost_reply(self):
        # urllib raises a response-phase timeout/reset raw, NOT wrapped in
        # URLError. The request may already have been acted on.
        for raw in (TimeoutError("timed out"), http.client.RemoteDisconnected("closed"),
                    ConnectionResetError("reset")):
            with unittest.mock.patch("urllib.request.urlopen", side_effect=raw):
                with self.assertRaises(rec.RecoveryPostError) as ctx:
                    rec.recovery_boot_guard_reset("10.0.0.5")
            self.assertIsNone(ctx.exception.status, raw)
            self.assertEqual(ctx.exception.stage, "post", raw)
            self.assertIn("outcome unknown", str(ctx.exception))


class QueryTest(unittest.TestCase):
    def test_pico_upload_query_builder(self):
        self.assertEqual(rec.pico_upload_query(0xDEADBEEF), "crc=deadbeef")
        self.assertEqual(rec.pico_upload_query(0x1, "B"), "crc=00000001&slot=B")
        for bad in ((-1, None), (1 << 32, None), (1, "C"), (1, "a")):
            with self.assertRaises(ValueError, msg=repr(bad)):
                rec.pico_upload_query(*bad)

    def test_bad_queries_refused_before_any_request(self):
        with unittest.mock.patch("urllib.request.urlopen") as m:
            for q in ("?crc=deadbeef", "crc=dead beef", "crc=dead#beef", "x" * 96):
                with self.assertRaises(ValueError, msg=q):
                    rec.post("10.0.0.5", "/x", query=q)
            with self.assertRaises(ValueError):
                rec.post("10.0.0.5", "/x?crc=1")
        m.assert_not_called()


if __name__ == "__main__":
    unittest.main()
