#!/usr/bin/env python3
"""Unit tests for mcp_server_info.kiln_config_apply() -- the MCP tool
wrapping POST /api/kiln_configs/apply + GET /api/kiln_configs/apply_status.
All against mocked kiln_configs_apply_http_client calls; no real socket, no
live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_kiln_config_apply.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import kiln_configs_apply_http_client as ac  # noqa: E402


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class DryRunTest(_Base):
    def test_dry_run_never_posts(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply") as post_mock:
            result = msi.kiln_config_apply(id=3, confirm=False)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()

    def test_dry_run_names_ack_flag(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply") as post_mock:
            result = msi.kiln_config_apply(id=3, confirm=False, ack_hardware_differs=True)
        self.assertIn("ack_hardware_differs=True", result)
        post_mock.assert_not_called()


class ConfirmedApplyTest(_Base):
    def test_applied_and_confirmed_passes(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(202, '{"ok":true}')) as post_mock, \
             unittest.mock.patch.object(ac, "poll_apply_status",
                                         return_value={"state": "done_ok", "id": 3, "diverged": False, "reason": ""}):
            result = msi.kiln_config_apply(id=3, confirm=True)
        post_mock.assert_called_once_with("10.0.0.5", 3, ack_hardware_differs=False)
        self.assertIn("ok - applied", result)

    def test_ack_hardware_differs_forwarded_to_client(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(202, '{"ok":true}')) as post_mock, \
             unittest.mock.patch.object(ac, "poll_apply_status",
                                         return_value={"state": "done_ok", "diverged": False}):
            msi.kiln_config_apply(id=3, confirm=True, ack_hardware_differs=True)
        post_mock.assert_called_once_with("10.0.0.5", 3, ack_hardware_differs=True)

    def test_diverged_result_fails_loud_even_if_state_done_ok(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(202, '{"ok":true}')), \
             unittest.mock.patch.object(ac, "poll_apply_status",
                                         return_value={"state": "done_ok", "diverged": True, "reason": "swap failed midway"}):
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("DIVERGED", result)

    def test_done_failed_reports_failed(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(202, '{"ok":true}')), \
             unittest.mock.patch.object(ac, "poll_apply_status",
                                         return_value={"state": "done_failed", "diverged": False, "reason": "interlock refused"}):
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("interlock refused", result)

    def test_non_terminal_state_reports_unknown_not_ok(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(202, '{"ok":true}')), \
             unittest.mock.patch.object(ac, "poll_apply_status",
                                         return_value={"state": "running", "diverged": False}):
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("UNKNOWN", result)
        self.assertNotIn("ok - applied", result)


class HardwareDiffersRefusalTest(_Base):
    def test_428_without_ack_names_hint_and_never_polls(self):
        """The whole point of this tool: a 428 must surface the board's own
        message verbatim, and must never be silently retried with the ack
        header on the caller's behalf."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(428, "hardware shape differs: ct_topology")), \
             unittest.mock.patch.object(ac, "poll_apply_status") as poll_mock:
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("428", result)
        self.assertIn("hardware shape differs: ct_topology", result)
        self.assertIn("ack_hardware_differs=True", result)
        poll_mock.assert_not_called()

    def test_404_no_such_config(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(404, "no such kiln config")):
            result = msi.kiln_config_apply(id=999, confirm=True)
        self.assertIn("404", result)
        self.assertIn("no such kiln config", result)

    def test_unexpected_status_is_an_error(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(409, "an apply is already running")):
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("error", result.lower())


class TransportFailureTest(_Base):
    def test_unreachable_host_is_an_error(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply",
                                         side_effect=ac.KilnConfigsApplyHttpError("unreachable")):
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("error", result.lower())

    def test_status_poll_failure_after_accepted_post_is_unknown_not_ok(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ac, "post_apply", return_value=(202, '{"ok":true}')), \
             unittest.mock.patch.object(ac, "poll_apply_status",
                                         side_effect=ac.KilnConfigsApplyHttpError("timed out")):
            result = msi.kiln_config_apply(id=3, confirm=True)
        self.assertIn("error", result.lower())
        self.assertNotIn("ok - applied", result)


if __name__ == "__main__":
    unittest.main()
