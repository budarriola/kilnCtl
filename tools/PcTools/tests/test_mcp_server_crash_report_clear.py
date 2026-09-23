#!/usr/bin/env python3
"""Unit tests for mcp_server_info.crash_report_clear() -- the MCP tool
that wraps GET /api/crash_report + GET /api/coredump/info + POST
/api/crash_report/clear. All against mocked dashboard_http_client/
coredump_fetch/crash_report_clear_http_client calls; no real socket, no
live board.

2026-09-22: extended for the opus-review advisory that the tool used to
gate entirely on the NVS crash record (``present``) and could not clear a
board with a stale coredump IMAGE but no crash record, even though
freeing the coredump partition is the tool's whole motivation. Every test
now also mocks ``coredump_fetch.get_coredump_info`` (real code always
calls it, even when the crash-record gate alone would already refuse or
short-circuit) plus three new cases covering image-only presence.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_crash_report_clear.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import coredump_fetch  # noqa: E402
from kilnctrl import crash_report_clear_http_client  # noqa: E402


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


def _image_info(present: bool, data_len: int = 0x1234, partition_size: int = 0x100000) -> coredump_fetch.CoredumpInfo:
    return coredump_fetch.CoredumpInfo(
        present=present,
        data_len=data_len if present else coredump_fetch.COREDUMP_BLANK_LEN,
        partition_size=partition_size,
        chunk_size=coredump_fetch.COREDUMP_HTTP_CHUNK_BYTES,
    )


_IMAGE_ABSENT = _image_info(False)
_IMAGE_PRESENT = _image_info(True)


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")

    def _image_patch(self, *values):
        """Patches coredump_fetch.get_coredump_info. A single CoredumpInfo
        answers every call; a sequence is consumed one call at a time (for
        tests exercising the pre-fetch AND the post-clear read-back)."""
        if len(values) == 1:
            return unittest.mock.patch.object(coredump_fetch, "get_coredump_info", return_value=values[0])
        remaining = list(values)

        def fake_get(host, timeout=5.0):
            return remaining.pop(0)

        return unittest.mock.patch.object(coredump_fetch, "get_coredump_info", side_effect=fake_get)


class NothingPendingTest(_Base):
    def test_no_record_no_image_does_nothing(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_NONE_PENDING) as get_mock, \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=True, allow_unacknowledged=True)
        self.assertIn("nothing pending", result)
        get_mock.assert_called_once()
        post_mock.assert_not_called()


class ImageOnlyPresentTest(_Base):
    """A stale coredump image with no NVS crash record (the advisory's
    motivating scenario) must be clearable -- this is the whole reason
    the tool exists, per its own docstring ("free the coredump partition
    before a reflash")."""

    def test_image_only_present_clears(self):
        get_calls = [_NONE_PENDING, _NONE_PENDING]

        def fake_get(host):
            return get_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         side_effect=fake_get), \
             self._image_patch(_IMAGE_PRESENT, _IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         return_value={"ok": True}) as post_mock:
            result = msi.crash_report_clear(confirm=True)
        post_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("ok - cleared and confirmed", result)
        self.assertIn("no crash record present", result)

    def test_image_only_dry_run_does_not_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_NONE_PENDING), \
             self._image_patch(_IMAGE_PRESENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=False)
        self.assertIn("DRY RUN", result)
        self.assertIn("present=True", result)
        post_mock.assert_not_called()

    def test_image_still_present_after_post_fails_loud(self):
        """The board can answer {"ok":true} and still leave the coredump
        image behind (e.g. hal_sysinfo_coredump_erase() failing silently
        from this tool's point of view because the POST's own 200 body
        carries no detail) -- this must never be reported as success."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_NONE_PENDING), \
             self._image_patch(_IMAGE_PRESENT, _IMAGE_PRESENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         return_value={"ok": True}):
            result = msi.crash_report_clear(confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("coredump image still present=true", result)
        self.assertNotIn("ok - cleared", result)


class UnacknowledgedGateTest(_Base):
    """A present-but-unacknowledged record must never be cleared without
    the explicit allow_unacknowledged=True escape hatch, regardless of
    confirm -- a bench agent must not erase an unreviewed crash."""

    def test_unacknowledged_refused_even_with_confirm(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=True)
        self.assertIn("REFUSED", result)
        self.assertIn("allow_unacknowledged", result)
        post_mock.assert_not_called()

    def test_unacknowledged_with_allow_flag_still_needs_confirm(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_UNACKED), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=False, allow_unacknowledged=True)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()


class DryRunTest(_Base):
    def test_dry_run_reports_and_does_not_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_ACKED), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=False)
        self.assertIn("DRY RUN", result)
        self.assertIn("safety_poll", result)
        self.assertIn("IllegalInstruction", result)
        post_mock.assert_not_called()


class ConfirmedClearTest(_Base):
    def test_confirmed_clear_posts_and_confirms_cleared(self):
        get_calls = [_PRESENT_ACKED, _NONE_PENDING]

        def fake_get(host):
            return get_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         side_effect=fake_get), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         return_value={"ok": True}) as post_mock:
            result = msi.crash_report_clear(confirm=True)
        post_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("ok - cleared and confirmed", result)
        self.assertIn("safety_poll", result)

    def test_clear_that_does_not_clear_fails_loud(self):
        """The board can answer {"ok":true} to the POST and STILL read
        back present=true on the immediate re-fetch -- this must never be
        reported as success (same class as boot_guard's write-lies bug
        per CLAUDE.md)."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_ACKED), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         return_value={"ok": True}):
            result = msi.crash_report_clear(confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("crash record still present=true", result)
        self.assertNotIn("ok - cleared", result)

    def test_500_erase_failure_is_reported_distinctly(self):
        err = crash_report_clear_http_client.CrashReportClearHttpError(
            "refused", status=500, detail="ESP_ERR_NOT_FOUND")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_ACKED), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         side_effect=err):
            result = msi.crash_report_clear(confirm=True)
        self.assertIn("500", result)
        self.assertIn("could not erase", result)

    def test_allow_unacknowledged_lets_confirm_clear_through(self):
        get_calls = [_PRESENT_UNACKED, _NONE_PENDING]

        def fake_get(host):
            return get_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         side_effect=fake_get), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         return_value={"ok": True}) as post_mock:
            result = msi.crash_report_clear(confirm=True, allow_unacknowledged=True)
        post_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("ok - cleared and confirmed", result)


class AuthFailureTest(_Base):
    def test_auth_failure_surfaces_as_error_not_success(self):
        err = crash_report_clear_http_client.CrashReportClearHttpError(
            "refused", status=401, detail="authentication required")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_ACKED), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear",
                                         side_effect=err):
            result = msi.crash_report_clear(confirm=True)
        self.assertIn("error", result.lower())
        self.assertNotIn("ok - cleared", result)


class UnreadableInitialFetchTest(_Base):
    def test_unreadable_pre_fetch_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 msi.dashboard_http_client, "get_crash_report",
                 side_effect=msi.dashboard_http_client.DashboardHttpError("unreachable")), \
             self._image_patch(_IMAGE_ABSENT), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=True, allow_unacknowledged=True)
        self.assertIn("error", result.lower())
        post_mock.assert_not_called()

    def test_unreadable_coredump_info_errors_before_any_post(self):
        """A record present but the image info fetch fails -- must not
        proceed on incomplete information."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi.dashboard_http_client, "get_crash_report",
                                         return_value=_PRESENT_ACKED), \
             unittest.mock.patch.object(
                 coredump_fetch, "get_coredump_info",
                 side_effect=coredump_fetch.CoredumpFetchError("unreachable")), \
             unittest.mock.patch.object(crash_report_clear_http_client, "post_crash_report_clear") as post_mock:
            result = msi.crash_report_clear(confirm=True, allow_unacknowledged=True)
        self.assertIn("error", result.lower())
        self.assertIn("coredump/info", result)
        post_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
