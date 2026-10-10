#!/usr/bin/env python3
"""Unit tests for mcp_server_profile_live's profile_live_* tools -- host
resolution, confirm-gating on the write tools, and error-string surfacing.
mcp_server_profile_live's imported `profile_live_http` module is mocked
directly; no real socket, no live board.

The confirm=False tests are the load-bearing ones: they assert the client
function is NEVER called when confirm is left at its default, i.e. no
request reaches the board.

Run with: python -m pytest tools/PcTools/tests/test_profile_live_mcp_tools.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_profile_live as mpl  # noqa: E402
from kilnctrl.wifi_uart import WifiUartQueryError  # noqa: E402



def _rb(kind):
    """Patch the live read-back GETs to show the write took effect."""
    import contextlib
    h = mpl.profile_live_http
    st = {"fork": {"active": True, "working_id": 250}}.get(kind, {"pending_decision": False, "working_id": -1})
    stack = contextlib.ExitStack()
    stack.enter_context(unittest.mock.patch.object(h, "get_live_status", return_value=st))
    stack.enter_context(unittest.mock.patch.object(
        h, "get_live_content", return_value={"name": "x", "zone_mask": 1, "segments": [{}]}))
    return stack


class ProfileLiveGetTests(unittest.TestCase):
    def test_status_reports_ok(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "get_live_status",
                                         return_value={"active": True, "working_id": 250}) as m:
            result = mpl.profile_live_get(host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        self.assertIn("250", result)
        m.assert_called_once_with("10.0.0.5")

    def test_content_flag_calls_content_endpoint(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "get_live_content",
                                         return_value={"id": 250, "name": "x"}) as m:
            result = mpl.profile_live_get(content=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        m.assert_called_once_with("10.0.0.5")

    def test_http_error_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mpl.profile_live_http, "get_live_content",
            side_effect=mpl.profile_live_http.ProfileLiveHttpError(
                "boom", status=409, detail="no working copy -- fork first"),
        ):
            result = mpl.profile_live_get(content=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("409", result)
        self.assertIn("no working copy", result)


class ProfileLiveForkConfirmGateTests(unittest.TestCase):
    def test_confirm_false_refuses_without_sending_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "fork_live") as m:
            result = mpl.profile_live_fork(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("confirm=True", result)
        m.assert_not_called()

    def test_confirm_true_sends_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "fork_live",
                                         return_value={"ok": True, "working_id": 250}) as m:
            with _rb("fork"):
                result = mpl.profile_live_fork(confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        m.assert_called_once_with("10.0.0.5")

    def test_nothing_running_reports_board_message(self):
        with unittest.mock.patch.object(
            mpl.profile_live_http, "fork_live",
            side_effect=mpl.profile_live_http.ProfileLiveHttpError(
                "boom", status=409, detail="no active firing to fork from"),
        ):
            result = mpl.profile_live_fork(confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("no active firing", result)


class ProfileLiveEditConfirmGateTests(unittest.TestCase):
    def test_confirm_false_refuses_without_sending_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "edit_live") as m:
            result = mpl.profile_live_edit("x", 1, [{"kind": 0, "target": 100, "ramp": 1, "dwell": 1}],
                                           host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("confirm=True", result)
        m.assert_not_called()

    def test_confirm_true_sends_request(self):
        segments = [{"kind": 0, "target": 100, "ramp": 1, "dwell": 1}]
        with unittest.mock.patch.object(mpl.profile_live_http, "edit_live",
                                         return_value={"ok": True, "warnings": []}) as m:
            with _rb("edit"):
                result = mpl.profile_live_edit("x", 1, segments, confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        m.assert_called_once_with("10.0.0.5", "x", 1, segments, generation=None)

    def test_unknown_segment_key_value_error_surfaces_as_error_string(self):
        with unittest.mock.patch.object(
            mpl.profile_live_http, "edit_live",
            side_effect=ValueError("segment 0: unknown key(s) ['bogus_field']"),
        ):
            result = mpl.profile_live_edit("x", 1, [{"kind": 0, "bogus_field": 1}], confirm=True,
                                           host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("bogus_field", result)

    def test_bound_violation_reports_board_message(self):
        with unittest.mock.patch.object(
            mpl.profile_live_http, "edit_live",
            side_effect=mpl.profile_live_http.ProfileLiveHttpError(
                "boom", status=400, detail="segment 2 target_c exceeds abs_max_temp_c"),
        ):
            result = mpl.profile_live_edit("x", 1, [{"kind": 0}], confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("abs_max_temp_c", result)


class ProfileLiveDecideConfirmGateTests(unittest.TestCase):
    def test_confirm_false_refuses_without_sending_request_for_discard(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "decide_live_discard") as m:
            result = mpl.profile_live_decide("discard", host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("confirm=True", result)
        m.assert_not_called()

    def test_confirm_true_discard_sends_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "decide_live_discard",
                                         return_value={"ok": True}) as m:
            with _rb("decide"):
                result = mpl.profile_live_decide("discard", confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        m.assert_called_once_with("10.0.0.5", generation=None)

    def test_save_as_without_name_refuses_without_sending_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "decide_live_save_as") as m:
            result = mpl.profile_live_decide("save_as", confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("name", result)
        m.assert_not_called()

    def test_save_as_with_name_sends_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "decide_live_save_as",
                                         return_value={"ok": True, "id": 3}) as m:
            with _rb("decide"):
                result = mpl.profile_live_decide("save_as", name="new one", confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        m.assert_called_once_with("10.0.0.5", "new one", generation=None)

    def test_generation_is_passed_through(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "decide_live_discard",
                                         return_value={"ok": True}) as m:
            mpl.profile_live_decide("discard", confirm=True, host="10.0.0.5", generation=12)
        m.assert_called_once_with("10.0.0.5", generation=12)

    def test_overwrite_sends_request(self):
        with unittest.mock.patch.object(mpl.profile_live_http, "decide_live_overwrite",
                                         return_value={"ok": True}) as m:
            with _rb("decide"):
                result = mpl.profile_live_decide("overwrite", confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        m.assert_called_once_with("10.0.0.5", generation=None)

    def test_overwrite_builtin_origin_reports_403(self):
        with unittest.mock.patch.object(
            mpl.profile_live_http, "decide_live_overwrite",
            side_effect=mpl.profile_live_http.ProfileLiveHttpError(
                "boom", status=403, detail="cannot overwrite a builtin profile"),
        ):
            result = mpl.profile_live_decide("overwrite", confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("403", result)
        self.assertIn("builtin", result)

    def test_unknown_action_refuses(self):
        result = mpl.profile_live_decide("delete_everything", confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("unknown action", result)


class ProfileLiveHostResolutionTests(unittest.TestCase):
    def test_explicit_host_bypasses_wifi_lookup(self):
        with unittest.mock.patch.object(mpl._srv, "_wifi") as mock_wifi, \
             unittest.mock.patch.object(mpl.profile_live_http, "get_live_status",
                                         return_value={"active": False}) as m:
            mpl.profile_live_get(host="kilnctl.local")
        mock_wifi.get_status.assert_not_called()
        m.assert_called_once_with("kilnctl.local")

    def test_no_host_falls_back_to_ap_default_when_sta_unreachable(self):
        with unittest.mock.patch.object(
            mpl._srv, "_wifi",
            **{"get_status.side_effect": WifiUartQueryError("no link")},
        ), unittest.mock.patch.object(mpl.profile_live_http, "get_live_status",
                                       return_value={"active": False}) as m:
            mpl.profile_live_get()
        m.assert_called_once_with("192.168.4.1")


class LiveWriteResultPrefixTests(unittest.TestCase):
    """A failed or unverified read-back must not read as "ok -" (review M4)."""

    def _run(self, status):
        h = mpl.profile_live_http
        with unittest.mock.patch.object(h, "fork_live", return_value={"ok": True}),                 unittest.mock.patch.object(h, "get_live_status", **status):
            return mpl.profile_live_fork(confirm=True, host="h")

    def test_failed_readback_is_not_ok(self):
        out = self._run({"return_value": {"active": False, "working_id": -1}})
        self.assertTrue(out.startswith("FAILED"), out)

    def test_unverified_readback_is_not_ok(self):
        out = self._run({"side_effect": OSError("x")})
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("UNVERIFIED", out)

    def test_edit_compares_zone_mask_and_segment_count(self):
        h = mpl.profile_live_http
        for body in ({"name": "x", "zone_mask": 2, "segments": [{}]},
                     {"name": "x", "zone_mask": 1, "segments": []}):
            with unittest.mock.patch.object(h, "edit_live", return_value={"ok": True}),                     unittest.mock.patch.object(h, "get_live_status", return_value={}),                     unittest.mock.patch.object(h, "get_live_content", return_value=body):
                out = mpl.profile_live_edit("x", 1, [{"kind": 0}], confirm=True, host="h")
            self.assertTrue(out.startswith("FAILED"), out)


if __name__ == "__main__":
    unittest.main()
