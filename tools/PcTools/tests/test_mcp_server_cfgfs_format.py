#!/usr/bin/env python3
"""Unit tests for mcp_server_info.cfgfs_format() -- the MCP tool wrapping
GET /api/cfgfs + POST /api/cfgfs/format_confirm. All against mocked
dashboard_http_client/ota_http_client calls; no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_cfgfs_format.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import dashboard_http_client  # noqa: E402
from kilnctrl import ota_http_client as ota_http  # noqa: E402


_BEFORE = {"mounted": True, "status": "mounted", "file_count": 7, "files": []}
_AFTER = {"mounted": True, "status": "mounted", "file_count": 0, "files": []}


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class DryRunTest(_Base):
    """Without confirm=True this must be a pure read -- no POST at all."""

    def test_dry_run_reports_file_count_and_never_posts(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(dashboard_http_client, "get_cfgfs_status", return_value=_BEFORE), \
             unittest.mock.patch.object(ota_http, "format_cfgfs") as format_mock:
            result = msi.cfgfs_format(confirm=False, password="hunter2")
        self.assertIn("DRY RUN", result)
        self.assertIn("7", result)
        format_mock.assert_not_called()

    def test_dry_run_never_reads_ap_password(self):
        """A dry run must not even try to resolve a credential -- omitting
        both `password` and KILNCTL_AP_PASSWORD must not raise."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(dashboard_http_client, "get_cfgfs_status", return_value=_BEFORE), \
             unittest.mock.patch.object(ota_http, "format_cfgfs") as format_mock, \
             unittest.mock.patch.dict(os.environ, {}, clear=True):
            os.environ.pop("KILNCTL_AP_PASSWORD", None)
            result = msi.cfgfs_format(confirm=False)
        self.assertIn("DRY RUN", result)
        format_mock.assert_not_called()


class ConfirmedFormatTest(_Base):
    def test_confirmed_format_posts_once_and_reports_before_after(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(dashboard_http_client, "get_cfgfs_status",
                                         side_effect=[_BEFORE, _AFTER]), \
             unittest.mock.patch.object(
                 ota_http, "format_cfgfs",
                 return_value={"ok": True, "status_code": 200,
                               "detail": "ok -- cfg partition formatted and mounted"}) as format_mock:
            result = msi.cfgfs_format(confirm=True, password="hunter2")
        format_mock.assert_called_once_with("10.0.0.5", "hunter2")
        self.assertIn("ok - cfg partition formatted", result)
        self.assertIn("before file_count=7", result)
        self.assertIn("after file_count=0", result)

    def test_no_password_available_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(dashboard_http_client, "get_cfgfs_status", return_value=_BEFORE), \
             unittest.mock.patch.object(ota_http, "format_cfgfs") as format_mock, \
             unittest.mock.patch.dict(os.environ, {}, clear=True):
            os.environ.pop("KILNCTL_AP_PASSWORD", None)
            result = msi.cfgfs_format(confirm=True)
        self.assertIn("error", result.lower())
        self.assertIn("KILNCTL_AP_PASSWORD", result)
        format_mock.assert_not_called()

    def test_500_format_failed_is_surfaced(self):
        err = ota_http.OtaHttpError("refused", status=500, detail="format failed: ESP_ERR_INVALID_STATE")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(dashboard_http_client, "get_cfgfs_status", return_value=_BEFORE), \
             unittest.mock.patch.object(ota_http, "format_cfgfs", side_effect=err):
            result = msi.cfgfs_format(confirm=True, password="hunter2")
        self.assertIn("error", result.lower())
        self.assertNotIn("ok - cfg partition formatted", result)

    def test_readback_failure_after_post_does_not_claim_success(self):
        """The POST succeeded but the confirming re-read failed -- must be
        reported as an unverified after-state, never a plain 'ok'."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 dashboard_http_client, "get_cfgfs_status",
                 side_effect=[_BEFORE, dashboard_http_client.DashboardHttpError("unreachable")]), \
             unittest.mock.patch.object(
                 ota_http, "format_cfgfs",
                 return_value={"ok": True, "status_code": 200, "detail": "ok -- formatted"}):
            result = msi.cfgfs_format(confirm=True, password="hunter2")
        self.assertIn("re-read failed", result)
        self.assertIn("UNKNOWN", result)


class UnreadableInitialFetchTest(_Base):
    def test_unreadable_initial_status_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 dashboard_http_client, "get_cfgfs_status",
                 side_effect=dashboard_http_client.DashboardHttpError("unreachable")), \
             unittest.mock.patch.object(ota_http, "format_cfgfs") as format_mock:
            result = msi.cfgfs_format(confirm=True, password="hunter2")
        self.assertIn("error", result.lower())
        format_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
