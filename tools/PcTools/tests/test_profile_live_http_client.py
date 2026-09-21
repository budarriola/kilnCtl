#!/usr/bin/env python3
"""Unit tests for kilnctrl.profile_live_http_client -- request construction
and response parsing, all against MOCKED urllib responses. No real socket
and no live board is used or required.

These tests do NOT exercise profiles_live_http.c on real hardware -- they
only check that this PC-side client builds the requests the firmware
documents (profiles_live_http.c) and parses the firmware's documented
{"ok":...} / {"ok":false,"error":...} response shapes correctly. Live-board
verification is still outstanding.

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

from kilnctrl import profile_live_http_client as plive  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _fake_http_error(status: int, body: bytes):
    return urllib.error.HTTPError("http://x/api/profile/live", status, "err", hdrs=None, fp=io.BytesIO(body))


class GetLiveStatusTest(unittest.TestCase):
    def test_parses_status_object(self):
        payload = {
            "active": True, "origin_id": 3, "origin_is_builtin": False,
            "working_id": 32, "editable_from_segment": 2, "pending_decision": False,
            "last_refusal": None,
        }
        body = json.dumps(payload).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.get_live_status("192.168.4.1")
        self.assertEqual(result, payload)

    def test_no_working_copy_is_minus_one_not_an_error(self):
        payload = {"active": False, "origin_id": 0, "origin_is_builtin": False,
                   "working_id": -1, "editable_from_segment": 0, "pending_decision": False,
                   "last_refusal": None}
        body = json.dumps(payload).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.get_live_status("192.168.4.1")
        self.assertEqual(result["working_id"], -1)

    def test_last_refusal_carries_generation(self):
        payload = {"active": False, "origin_id": 0, "origin_is_builtin": False,
                   "working_id": -1, "editable_from_segment": 0, "pending_decision": False,
                   "last_refusal": {"generation": 7, "result": 2, "message": "window violation"}}
        body = json.dumps(payload).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.get_live_status("192.168.4.1")
        self.assertEqual(result["last_refusal"]["generation"], 7)

    def test_non_json_body_raises(self):
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(plive.ProfileLiveHttpError):
                plive.get_live_status("192.168.4.1")


class GetLiveContentTest(unittest.TestCase):
    def test_parses_segments(self):
        payload = {"id": 250, "name": "fork", "zone_mask": 3, "segment_count": 1,
                   "segments": [{"seg_kind": 0, "target_c": 1000.0, "ramp_c_per_hr": 120.0,
                                 "dwell_min": 10, "io_target": 0, "io_state": 0,
                                 "io_blocking": 0, "io_leave_on_at_end": 0}]}
        body = json.dumps(payload).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.get_live_content("192.168.4.1")
        self.assertEqual(result["segments"][0]["target_c"], 1000.0)

    def test_no_working_copy_is_409_with_board_message(self):
        err_body = json.dumps({"ok": False, "error": "no working copy -- fork first"}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         side_effect=_fake_http_error(409, err_body)):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.get_live_content("192.168.4.1")
        self.assertEqual(ctx.exception.status, 409)
        self.assertEqual(ctx.exception.detail, "no working copy -- fork first")


class ForkLiveTest(unittest.TestCase):
    def test_parses_ok_response(self):
        body = json.dumps({"ok": True, "origin_id": 5, "working_id": 250}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.fork_live("192.168.4.1")
        self.assertEqual(result["working_id"], 250)

    def test_nothing_running_is_409(self):
        err_body = json.dumps({"ok": False, "error": "no active firing to fork from"}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         side_effect=_fake_http_error(409, err_body)):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.fork_live("192.168.4.1")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("no active firing", ctx.exception.detail)

    def test_post_body_is_empty(self):
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data
            captured["method"] = req.get_method()
            return _fake_response(json.dumps({"ok": True, "origin_id": 1, "working_id": 250}).encode())

        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.fork_live("192.168.4.1")
        self.assertEqual(captured["data"], b"")
        self.assertEqual(captured["method"], "POST")


class EditLiveTest(unittest.TestCase):
    def test_builds_zone_ramp_segment_fields(self):
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "warnings": []}).encode())

        segments = [{"kind": 0, "target": 1000, "ramp": 100, "dwell": 30}]
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            result = plive.edit_live("192.168.4.1", "test", 3, segments)
        self.assertEqual(result, {"ok": True, "warnings": []})
        body = captured["data"]
        self.assertIn("name=test", body)
        self.assertIn("zone_mask=3", body)
        self.assertIn("seg_count=1", body)
        self.assertIn("seg0_target=1000", body)
        self.assertIn("seg0_ramp=100", body)
        self.assertIn("seg0_dwell=30", body)
        # No `id` field -- the live route always targets the implicit working slot.
        self.assertNotIn("id=", body.replace("zone_mask", "").replace("seg_count", ""))

    def test_builds_io_segment_fields(self):
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "warnings": []}).encode())

        segments = [{"kind": 1, "io_target": 2, "io_state": 1, "io_blocking": 1, "io_leave_on": 0}]
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.edit_live("192.168.4.1", "iotest", 1, segments)
        body = captured["data"]
        self.assertIn("seg0_io_target=2", body)
        self.assertIn("seg0_io_state=1", body)
        self.assertIn("seg0_io_blocking=1", body)
        self.assertIn("seg0_io_leave_on=0", body)

    def test_bound_violation_is_400_with_board_message(self):
        err_body = json.dumps({"ok": False, "error": "segment 2 target_c exceeds abs_max_temp_c"}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         side_effect=_fake_http_error(400, err_body)):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.edit_live("192.168.4.1", "x", 1, [{"kind": 0, "target": 9999, "ramp": 1, "dwell": 1}])
        self.assertEqual(ctx.exception.status, 400)
        self.assertIn("abs_max_temp_c", ctx.exception.detail)

    def test_window_violation_is_409_with_board_message(self):
        err_body = json.dumps({"ok": False, "error": "segment 0 has already started"}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         side_effect=_fake_http_error(409, err_body)):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.edit_live("192.168.4.1", "x", 1, [{"kind": 0, "target": 100, "ramp": 1, "dwell": 1}])
        self.assertEqual(ctx.exception.status, 409)


class DecideLiveTest(unittest.TestCase):
    def test_discard_ok(self):
        body = json.dumps({"ok": True}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.decide_live_discard("192.168.4.1")
        self.assertEqual(result, {"ok": True})

    def test_discard_sends_action_field(self):
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True}).encode())

        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.decide_live_discard("192.168.4.1")
        self.assertEqual(captured["data"], "action=discard")

    def test_nothing_pending_is_409(self):
        err_body = json.dumps({"ok": False, "error": "nothing pending"}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         side_effect=_fake_http_error(409, err_body)):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.decide_live_discard("192.168.4.1")
        self.assertEqual(ctx.exception.status, 409)

    def test_save_as_returns_new_id(self):
        body = json.dumps({"ok": True, "id": 3}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", return_value=_fake_response(body)):
            result = plive.decide_live_save_as("192.168.4.1", "my new profile")
        self.assertEqual(result["id"], 3)

    def test_save_as_sends_name_field(self):
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "id": 3}).encode())

        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.decide_live_save_as("192.168.4.1", "my new profile")
        self.assertIn("action=save_as", captured["data"])
        self.assertIn("name=my+new+profile", captured["data"])

    def test_overwrite_sends_confirm_1(self):
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True}).encode())

        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.decide_live_overwrite("192.168.4.1")
        self.assertIn("action=overwrite", captured["data"])
        self.assertIn("confirm=1", captured["data"])

    def test_overwrite_builtin_origin_is_403(self):
        err_body = json.dumps({"ok": False, "error": "cannot overwrite a builtin profile"}).encode()
        with unittest.mock.patch.object(plive.urllib.request, "urlopen",
                                         side_effect=_fake_http_error(403, err_body)):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.decide_live_overwrite("192.168.4.1")
        self.assertEqual(ctx.exception.status, 403)
        self.assertIn("builtin", ctx.exception.detail)


if __name__ == "__main__":
    unittest.main()
