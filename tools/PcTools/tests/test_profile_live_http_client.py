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


class GenerationQueryTest(unittest.TestCase):
    """M1: generation is sent as ?gen=N on edit and every decide; omitted = no query."""

    def _urls(self, call):
        seen = []

        def _capture(req, timeout=None):
            seen.append(req.full_url)
            return _fake_response(json.dumps({"ok": True}).encode())

        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            call()
        return seen

    def test_edit_sends_gen(self):
        seg = [{"target": 100, "ramp": 50, "dwell": 1}]
        u = self._urls(lambda: plive.edit_live("h", "n", 1, seg, generation=7))
        self.assertTrue(u[0].endswith("/api/profile/live?gen=7"), u)

    def test_edit_without_gen_has_no_query(self):
        seg = [{"target": 100, "ramp": 50, "dwell": 1}]
        u = self._urls(lambda: plive.edit_live("h", "n", 1, seg))
        self.assertTrue(u[0].endswith("/api/profile/live"), u)

    def test_decide_variants_send_gen(self):
        for fn, args in ((plive.decide_live_discard, ()), (plive.decide_live_save_as, ("x",)),
                         (plive.decide_live_overwrite, ())):
            u = self._urls(lambda fn=fn, args=args: fn("h", *args, generation=3))
            self.assertTrue(u[0].endswith("/api/profile/live/decide?gen=3"), (fn.__name__, u))

    def test_stale_gen_409_surfaces_board_text(self):
        err = _fake_http_error(409, b'{"ok":false,"error":"working copy changed elsewhere -- reload"}')
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(plive.ProfileLiveHttpError) as ctx:
                plive.decide_live_discard("h", generation=1)
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("changed elsewhere", ctx.exception.detail)


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

    def test_accepts_get_live_content_spelling_for_zone_ramp(self):
        # A get_live_content()-shaped segment (target_c/ramp_c_per_hr/dwell_min)
        # must round-trip straight into edit_live without renaming keys.
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "warnings": []}).encode())

        segments = [{"kind": 0, "target_c": 1000, "ramp_c_per_hr": 100, "dwell_min": 30}]
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.edit_live("192.168.4.1", "test", 3, segments)
        body = captured["data"]
        self.assertIn("seg0_target=1000", body)
        self.assertIn("seg0_ramp=100", body)
        self.assertIn("seg0_dwell=30", body)

    def test_accepts_get_live_content_spelling_for_io_leave_on(self):
        # A get_live_content()-shaped io segment uses io_leave_on_at_end;
        # this must map to the wire field seg%u_io_leave_on faithfully,
        # not silently default to 0 (the bug this fix replaces).
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "warnings": []}).encode())

        segments = [{"kind": 1, "io_target": 2, "io_state": 1, "io_blocking": 1, "io_leave_on_at_end": 1}]
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.edit_live("192.168.4.1", "iotest", 1, segments)
        body = captured["data"]
        self.assertIn("seg0_io_leave_on=1", body)

    def test_missing_dwell_omits_the_field_rather_than_sending_zero(self):
        # PROFILE_RAMP_C_PER_HR_MIN/PROFILE_TARGET_C_MIN are 0.0f, so an
        # explicit 0 for an omitted field would be silently accepted by the
        # firmware instead of triggering its own "missing" 400 -- the field
        # must simply not be sent.
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "warnings": []}).encode())

        segments = [{"kind": 0, "target": 1000, "ramp": 100}]  # no dwell/dwell_min at all
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.edit_live("192.168.4.1", "test", 3, segments)
        body = captured["data"]
        self.assertIn("seg0_target=1000", body)
        self.assertIn("seg0_ramp=100", body)
        self.assertNotIn("seg0_dwell=", body)

    def test_missing_io_blocking_omits_the_field_rather_than_sending_zero(self):
        # Firmware defaults a missing seg%u_io_blocking to 1 (safe/blocking);
        # sending an explicit 0 would silently turn the segment non-blocking.
        captured = {}

        def _capture(req, timeout=None):
            captured["data"] = req.data.decode()
            return _fake_response(json.dumps({"ok": True, "warnings": []}).encode())

        segments = [{"kind": 1, "io_target": 2, "io_state": 1}]  # no io_blocking at all
        with unittest.mock.patch.object(plive.urllib.request, "urlopen", side_effect=_capture):
            plive.edit_live("192.168.4.1", "iotest", 1, segments)
        body = captured["data"]
        self.assertIn("seg0_io_target=2", body)
        self.assertIn("seg0_io_state=1", body)
        self.assertNotIn("seg0_io_blocking=", body)

    def test_unknown_segment_key_raises_value_error(self):
        segments = [{"kind": 0, "target": 1000, "ramp": 100, "dwell": 30, "bogus_field": 1}]
        with self.assertRaises(ValueError) as ctx:
            plive.edit_live("192.168.4.1", "test", 3, segments)
        self.assertIn("bogus_field", str(ctx.exception))

    def test_unknown_segment_key_never_sends_a_request(self):
        segments = [{"io_target": 2, "io_state": 1, "io_leave_on_at_end": 1, "typo_leave_on": 1}]
        with unittest.mock.patch.object(plive.urllib.request, "urlopen") as mock_urlopen:
            with self.assertRaises(ValueError):
                plive.edit_live("192.168.4.1", "x", 1, segments)
        mock_urlopen.assert_not_called()


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


class HttpAuthSeamTest(unittest.TestCase):
    def test_get_live_status_goes_through_http_auth_urlopen(self):
        # Every request must go through http_auth.urlopen() (the ADMIN-tier
        # session/login seam), not urllib.request.urlopen directly -- that is
        # what lets a 401 under web auth log in and retry transparently.
        body = json.dumps({"active": False, "origin_id": 0, "origin_is_builtin": False,
                            "working_id": -1, "editable_from_segment": 0,
                            "pending_decision": False, "last_refusal": None}).encode()
        with unittest.mock.patch.object(plive.http_auth, "urlopen",
                                         return_value=_fake_response(body)) as mock_auth_urlopen:
            plive.get_live_status("192.168.4.1")
        mock_auth_urlopen.assert_called_once()


if __name__ == "__main__":
    unittest.main()
