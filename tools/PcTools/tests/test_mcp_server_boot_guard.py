#!/usr/bin/env python3
"""Unit tests for mcp_server_info.boot_guard_get() -- the READ-ONLY MCP tool
that wraps GET /api/boot_guard via ota_http_client.get_boot_guard_status().
All against a mocked ota_http_client.get_boot_guard_status call; no real
socket, no live board, no credentials.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_boot_guard.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import ota_http_client  # noqa: E402


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class RenderTest(_Base):
    def test_renders_counter_and_recovery_mode_false(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ota_http_client, "get_boot_guard_status",
                                         return_value={"boot_count": 3, "recovery_mode": False}) as get_mock:
            result = msi.boot_guard_get()
        get_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("host=10.0.0.5", result)
        self.assertIn("boot_count=3", result)
        self.assertIn("recovery_mode=False", result)
        self.assertIn("summary: counter 3, recovery_mode False", result)

    def test_renders_recovery_mode_true(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(ota_http_client, "get_boot_guard_status",
                                         return_value={"boot_count": 7, "recovery_mode": True}):
            result = msi.boot_guard_get()
        self.assertIn("boot_count=7", result)
        self.assertIn("recovery_mode=True", result)

    def test_explicit_host_bypasses_resolution(self):
        with unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host",
                                         return_value="192.168.1.156") as resolve_mock, \
             unittest.mock.patch.object(ota_http_client, "get_boot_guard_status",
                                         return_value={"boot_count": 0, "recovery_mode": False}):
            result = msi.boot_guard_get(host="192.168.1.156")
        resolve_mock.assert_called_once_with("192.168.1.156")
        self.assertIn("host=192.168.1.156", result)


class ErrorTest(_Base):
    def test_http_error_is_reported_not_raised(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 ota_http_client, "get_boot_guard_status",
                 side_effect=ota_http_client.OtaHttpError("unreachable")):
            result = msi.boot_guard_get()
        self.assertIn("error", result.lower())


if __name__ == "__main__":
    unittest.main()
