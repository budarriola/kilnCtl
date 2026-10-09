#!/usr/bin/env python3
"""Unit tests for mcp_server_info.kiln_configs_quarantine_clear() -- the MCP
tool wrapping GET /api/kiln_configs + the non-mutating status probe +
POST /api/kiln_configs/quarantine_clear. All against mocked
kiln_configs_quarantine_http_client calls; no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_kiln_configs_quarantine_clear.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import kiln_configs_quarantine_http_client as qc  # noqa: E402


_LISTING = {"active_id": 1, "configs": [{"id": 1, "name": "cone6", "is_active": True}], "max_count": 8}


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class NotQuarantinedTest(_Base):
    def test_not_quarantined_does_nothing(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(qc, "get_kiln_configs_list", return_value=_LISTING), \
             unittest.mock.patch.object(qc, "get_quarantine_status",
                                         return_value=(False, "kiln config store is not quarantined")), \
             unittest.mock.patch.object(qc, "post_quarantine_clear") as post_mock:
            result = msi.kiln_configs_quarantine_clear(confirm=True)
        self.assertIn("not quarantined", result)
        post_mock.assert_not_called()


class DryRunTest(_Base):
    """Without confirm=True, a quarantined store must be reported but never
    cleared."""

    def test_dry_run_reports_and_does_not_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(qc, "get_kiln_configs_list", return_value=_LISTING), \
             unittest.mock.patch.object(qc, "get_quarantine_status",
                                         return_value=(True, "confirm=1 required to discard the quarantined store")), \
             unittest.mock.patch.object(qc, "post_quarantine_clear") as post_mock:
            result = msi.kiln_configs_quarantine_clear(confirm=False)
        self.assertIn("DRY RUN", result)
        self.assertIn("quarantined", result)
        post_mock.assert_not_called()


class ConfirmedClearTest(_Base):
    def test_confirmed_clear_posts_and_confirms_cleared(self):
        status_calls = [(True, "confirm=1 required"), (False, "not quarantined")]

        def fake_status(host):
            return status_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(qc, "get_kiln_configs_list", return_value=_LISTING), \
             unittest.mock.patch.object(qc, "get_quarantine_status", side_effect=fake_status), \
             unittest.mock.patch.object(qc, "post_quarantine_clear",
                                         return_value={"ok": True, "discarded_reason": "wrong size"}) as post_mock:
            result = msi.kiln_configs_quarantine_clear(confirm=True)
        post_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("ok - quarantine cleared and confirmed", result)

    def test_clear_that_does_not_take_fails_loud(self):
        """The board can answer {"ok":true} to the POST and the re-probe
        STILL reads quarantined -- this must never be reported as success
        (same class as boot_guard's write-lies bug per CLAUDE.md)."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(qc, "get_kiln_configs_list", return_value=_LISTING), \
             unittest.mock.patch.object(qc, "get_quarantine_status",
                                         return_value=(True, "confirm=1 required")), \
             unittest.mock.patch.object(qc, "post_quarantine_clear",
                                         return_value={"ok": True}):
            result = msi.kiln_configs_quarantine_clear(confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - quarantine cleared", result)

    def test_409_race_during_post_is_surfaced(self):
        err = qc.KilnConfigsQuarantineHttpError("refused", status=409, detail="not quarantined")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(qc, "get_kiln_configs_list", return_value=_LISTING), \
             unittest.mock.patch.object(qc, "get_quarantine_status",
                                         return_value=(True, "confirm=1 required")), \
             unittest.mock.patch.object(qc, "post_quarantine_clear", side_effect=err):
            result = msi.kiln_configs_quarantine_clear(confirm=True)
        self.assertIn("error", result.lower())
        self.assertNotIn("ok - quarantine cleared", result)


class UnreadableInitialFetchTest(_Base):
    def test_unreadable_list_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 qc, "get_kiln_configs_list",
                 side_effect=qc.KilnConfigsQuarantineHttpError("unreachable")), \
             unittest.mock.patch.object(qc, "get_quarantine_status") as status_mock, \
             unittest.mock.patch.object(qc, "post_quarantine_clear") as post_mock:
            result = msi.kiln_configs_quarantine_clear(confirm=True)
        self.assertIn("error", result.lower())
        status_mock.assert_not_called()
        post_mock.assert_not_called()

    def test_unreadable_status_probe_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(qc, "get_kiln_configs_list", return_value=_LISTING), \
             unittest.mock.patch.object(
                 qc, "get_quarantine_status",
                 side_effect=qc.KilnConfigsQuarantineHttpError("unreachable")), \
             unittest.mock.patch.object(qc, "post_quarantine_clear") as post_mock:
            result = msi.kiln_configs_quarantine_clear(confirm=True)
        self.assertIn("error", result.lower())
        post_mock.assert_not_called()


if __name__ == "__main__":
    unittest.main()
