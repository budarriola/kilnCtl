#!/usr/bin/env python3
"""Unit tests for mcp_server_info.get_readiness() -- the MCP tool that wraps
GET /api/readiness. All against a mocked readiness_http_client.get_readiness
call; no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_readiness.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import readiness_http_client  # noqa: E402


_SAMPLE = {
    "items": [
        {"key": "network", "label": "Network configured", "status": "ok",
         "detail": "connected to MyWifi", "fix_url": "/wifi"},
        {"key": "estop_verified", "label": "E-stop interlock verified", "status": "not_done",
         "detail": "never verified", "fix_url": "/safety"},
        {"key": "calibration", "label": "Thermocouple calibration offsets", "status": "cannot_yet",
         "detail": "blocked on thermo_count", "fix_url": ""},
    ]
}


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class RenderTest(_Base):
    def test_renders_each_item_and_summary(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_SAMPLE) as get_mock:
            result = msi.get_readiness()
        get_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("host=10.0.0.5", result)
        self.assertIn("ok network: connected to MyWifi", result)
        self.assertIn("not_done estop_verified: never verified", result)
        self.assertIn("(fix: /safety)", result)
        self.assertIn("cannot_yet calibration: blocked on thermo_count", result)
        self.assertIn("summary: 1 ok, 1 not_done, 1 other (3 total)", result)

    def test_empty_items_reports_zero_counts(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value={"items": []}):
            result = msi.get_readiness()
        self.assertIn("summary: 0 ok, 0 not_done, 0 other (0 total)", result)


class ErrorTest(_Base):
    def test_http_error_is_reported_not_raised(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 readiness_http_client, "get_readiness",
                 side_effect=readiness_http_client.ReadinessHttpError("unreachable")):
            result = msi.get_readiness()
        self.assertIn("error", result.lower())


if __name__ == "__main__":
    unittest.main()
