#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_test_trip() and its HTTP client, plus the
absent-trip_seq codec pin. Fake board only; no socket.

Run with: python -m pytest tools/PcTools/tests/test_safety_test_trip.py -q
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

from kilnctrl import kilnlink_codec, mcp_server, safety_test_trip_http_client as cl  # noqa: E402


def _diag(reason=0, mask=0, ever=True):
    return unittest.mock.MagicMock(ever_received=ever, trip_reason=reason, trip_mask=mask)


def _run(post, diags, confirm=True):
    with unittest.mock.patch.object(mcp_server._safety, "get_diag", side_effect=diags) as gd, \
         unittest.mock.patch("kilnctrl.mcp_server_ota._ota_resolve_host", return_value="h"), \
         unittest.mock.patch.object(cl, "post_test_trip", return_value=post) as pt, \
         unittest.mock.patch("time.sleep"):
        out = mcp_server.safety_test_trip(confirm=confirm)
    return out, gd, pt


class ToolTests(unittest.TestCase):
    def test_confirm_must_be_exactly_true(self):
        for c in (False, 1, "yes"):
            out, gd, pt = _run((200, {"ok": True}), [_diag()], confirm=c)
            self.assertTrue(out.startswith("refused"))
            pt.assert_not_called()
            gd.assert_not_called()

    def test_refuses_when_already_tripped(self):
        out, _, pt = _run((200, {}), [_diag(6, 0x20)])
        self.assertIn("already tripped", out)
        pt.assert_not_called()

    def test_refuses_without_diag(self):
        out, _, pt = _run((200, {}), [_diag(ever=False)])
        self.assertIn("no DIAG", out)
        pt.assert_not_called()

    def test_success_verifies_reason_and_mask(self):
        out, _, pt = _run((200, {"ok": True, "trip_seq": 7}), [_diag(), _diag(4, 0x0008)])
        self.assertTrue(out.startswith("ok"))
        self.assertIn("trip_seq=7", out)
        pt.assert_called_once()

    def test_readback_mismatch_fails(self):
        with unittest.mock.patch("time.monotonic", side_effect=[0.0, 99.0, 99.0, 99.0]):
            out, _, _ = _run((200, {"ok": True, "trip_seq": 7}), [_diag(), _diag(6, 0x20)])
        self.assertTrue(out.startswith("FAILED VERIFY"))

    def test_outcomes(self):
        out, _, _ = _run((503, {"ok": False, "outcome": "no_reply"}), [_diag()])
        self.assertIn("may predate TEST_TRIP", out)
        self.assertTrue(out.startswith("NOT CONFIRMED"))
        out, _, _ = _run((501, {"ok": False, "outcome": "pico_protocol_too_old", "pico_protocol": 17}), [_diag()])
        self.assertIn("predates TEST_TRIP", out)
        out, _, _ = _run((409, {"ok": False, "outcome": "rate_limited"}), [_diag()])
        self.assertIn("rate_limited", out)
        self.assertTrue(out.startswith("refused"))


class ClientTests(unittest.TestCase):
    def test_http_error_body_parsed(self):
        err = urllib.error.HTTPError("u", 409, "c", {}, io.BytesIO(b'{"ok":false,"outcome":"rate_limited"}'))
        with unittest.mock.patch("kilnctrl.http_auth.urlopen", side_effect=err) as uo:
            status, body = cl.post_test_trip("h")
        self.assertEqual((status, body["outcome"]), (409, "rate_limited"))
        self.assertEqual(uo.call_args[0][0].data, b"confirm=1")

    def test_non_json_raises(self):
        err = urllib.error.HTTPError("u", 500, "c", {}, io.BytesIO(b"oops"))
        with unittest.mock.patch("kilnctrl.http_auth.urlopen", side_effect=err):
            with self.assertRaises(cl.SafetyTestTripHttpError):
                cl.post_test_trip("h")


class CodecPinTests(unittest.TestCase):
    def test_absent_trip_seq_encodings(self):
        f = {"trip_mask": 8, "pico_boot_id": 0x11}
        d = dict(trip_reason=4, warn_mask=0, trip_mask=8, uptime_ms=1, boot_reason=0, context_age_100ms=0,
                 context_frames_ok=0, context_frames_bad=0, tx_frames_dropped=0, state=0, flags=0,
                 log_frames_dropped=0)
        base = kilnlink_codec.encode_diag(d)
        self.assertEqual(len(base), 30)  # no trip_seq / boot id -> legacy 30-byte frame
        self.assertEqual(len(kilnlink_codec.encode_diag({**d, "trip_seq": 5, "pico_boot_id": 9})), 32)
        self.assertEqual(kilnlink_codec.encode_clear_trip(f).hex(), "0a08000011")


if __name__ == "__main__":
    unittest.main()
