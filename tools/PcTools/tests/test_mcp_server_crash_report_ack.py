#!/usr/bin/env python3
"""Unit tests for mcp_server_info.crash_report_ack() -- the MCP tool that
wraps GET /api/crash_report + POST /api/crash_report/ack. All against
mocked dashboard_http_client/crash_report_ack_http_client calls; no real
socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_crash_report_ack.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import crash_report_ack_http_client  # noqa: E402


_PRESENT_UNACKED = {
    "present": True,
    "acknowledged": False,
    "exc_cause": 6,
    "exc_cause_str": "IllegalInstruction",
    "exc_pc": "0x4200abcd",
    "exc_addr": "0x00000000",
    "exc_task": "safety_poll",
    "found_on_boot_reset_reason": "panic/exception",
    "frame_trustworthy": True,
    "backtrace": ["0x4200abcd"],
    "backtrace_corrupted": False,
    "dump_id": 305419896,
    "fw_build": "Sep 22 2026 09:00:00",
    "crash_uptime_s": 17,
    "crash_uptime_known": True,
}

_PRESENT_ACKED = dict(_PRESENT_UNACKED, acknowledged=True)
_NONE_PENDING = {"present": False}


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class NothingPendingTest(_Base):
    def test_no_record_does_nothing(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_NONE_PENDING) as get_mock, \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack") as post_mock:
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("nothing pending", result)
        get_mock.assert_called_once()
        post_mock.assert_not_called()

    def test_already_acknowledged_does_nothing(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_ACKED), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack") as post_mock:
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("already acknowledged", result)
        post_mock.assert_not_called()


class DryRunTest(_Base):
    """Without confirm=True, a pending unacknowledged record must be
    reported -- summary included -- but never acknowledged."""

    def test_dry_run_reports_and_does_not_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack") as post_mock:
            result = msi.crash_report_ack(confirm=False)
        self.assertIn("DRY RUN", result)
        self.assertIn("safety_poll", result)
        self.assertIn("IllegalInstruction", result)
        self.assertIn("Sep 22 2026 09:00:00", result)
        self.assertIn("dump_id=305419896", result)
        self.assertIn("crash_uptime_s=17", result)
        post_mock.assert_not_called()


class ConfirmedAckTest(_Base):
    def test_confirmed_ack_posts_and_confirms_cleared(self):
        get_calls = [_PRESENT_UNACKED, _PRESENT_ACKED]

        def fake_get(host):
            return get_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         side_effect=fake_get), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack",
                                         return_value={"ok": True}) as post_mock:
            result = msi.crash_report_ack(confirm=True)
        post_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("ok - acknowledged and confirmed", result)
        self.assertIn("safety_poll", result)

    def test_ack_that_does_not_clear_fails_loud(self):
        """The board can answer {"ok":true} to the POST and STILL read back
        acknowledged=false on the immediate re-fetch -- this must never be
        reported as success (same class as boot_guard's write-lies bug per
        CLAUDE.md)."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack",
                                         return_value={"ok": True}):
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - acknowledged", result)

    def test_409_conflict_is_reported_distinctly(self):
        err = crash_report_ack_http_client.CrashReportAckHttpError(
            "refused", status=409, detail="no crash record to acknowledge")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack",
                                         side_effect=err):
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("409", result)
        self.assertIn("no crash record to acknowledge", result)

    def test_500_persist_failure_is_reported_distinctly(self):
        err = crash_report_ack_http_client.CrashReportAckHttpError(
            "refused", status=500, detail="failed to persist acknowledgement")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack",
                                         side_effect=err):
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("500", result)
        self.assertIn("could not persist", result)


class AuthFailureTest(_Base):
    def test_auth_failure_surfaces_as_error_not_success(self):
        err = crash_report_ack_http_client.CrashReportAckHttpError(
            "refused", status=401, detail="authentication required")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack",
                                         side_effect=err):
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("error", result.lower())
        self.assertNotIn("ok - acknowledged", result)


class UnreadableInitialFetchTest(_Base):
    def test_unreadable_pre_fetch_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 msi.dashboard_http_client, "get_crash_report",
                 side_effect=msi.dashboard_http_client.DashboardHttpError("unreachable")), \
             unittest.mock.patch.object(crash_report_ack_http_client, "post_crash_report_ack") as post_mock:
            result = msi.crash_report_ack(confirm=True)
        self.assertIn("error", result.lower())
        post_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
