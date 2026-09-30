#!/usr/bin/env python3
"""Unit tests for mcp_server_ota.ota_rollback_esp() -- the "deliberately
revert the ESP32-S3 to its previous firmware image right now" tool
(POST /api/ota/esp/rollback). This is destructive-adjacent (the board
reboots into different code) and, per its own docstring, has never been
exercised against real hardware -- so the string-formatting/error-path
logic exercised here is the only test coverage this tool has at all.

2026-09-29: this route's AP-password HMAC (WEB_AUTH_PLAN.md item 2b) was
retired -- ROUTE_TIER_ADMIN (the ordinary admin session) is the only gate
now, on or off. `ota_rollback_esp()` and `sw_reset_esp()` no longer take a
`password` argument and no longer read any KILNCTL_AP_PASSWORD fallback.

Same convention as test_ota_status_protocol_version.py: mcp_server_ota's
imported `ota_http` module is mocked directly, no real socket, no live
board. Also proves the "no previous valid image" and "wrong host resolution
falls back to AP default" cases, and that an OtaHttpError from the HTTP
layer surfaces as an "error:" string rather than an uncaught exception
reaching an MCP caller.

Run with: python -m pytest tools/PcTools/tests/test_ota_rollback_esp_mcp.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota as mo  # noqa: E402
from kilnctrl.wifi_uart import WifiUartQueryError  # noqa: E402


class OtaRollbackEspHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_and_previous_version(self):
        with unittest.mock.patch.object(
            mo.ota_http, "rollback_esp",
            return_value={"ok": True, "version_before": "1.4.2"},
        ) as mock_rollback:
            result = mo.ota_rollback_esp(host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        self.assertIn("1.4.2", result)
        self.assertIn("rebooting", result)
        mock_rollback.assert_called_once_with("10.0.0.5")


class OtaRollbackEspRefusalTests(unittest.TestCase):
    def test_no_previous_image_reports_board_reason(self):
        with unittest.mock.patch.object(
            mo.ota_http, "rollback_esp",
            return_value={"ok": False, "reason": "no previous valid image to roll back to"},
        ):
            result = mo.ota_rollback_esp(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("no previous valid image to roll back to", result)


class OtaRollbackEspHostResolutionTests(unittest.TestCase):
    def test_explicit_host_bypasses_wifi_lookup(self):
        with unittest.mock.patch.object(mo._srv, "_wifi") as mock_wifi, \
             unittest.mock.patch.object(
                 mo.ota_http, "rollback_esp", return_value={"ok": True, "version_before": "1.0.0"}
             ) as mock_rollback:
            mo.ota_rollback_esp(host="kilnctl.local")
        mock_wifi.get_status.assert_not_called()
        mock_rollback.assert_called_once_with("kilnctl.local")

    def test_no_host_falls_back_to_ap_default_when_sta_unreachable(self):
        with unittest.mock.patch.object(
            mo._srv, "_wifi",
            **{"get_status.side_effect": WifiUartQueryError("no link")},
        ), unittest.mock.patch.object(
            mo.ota_http, "rollback_esp", return_value={"ok": True, "version_before": "1.0.0"}
        ) as mock_rollback:
            mo.ota_rollback_esp()
        mock_rollback.assert_called_once_with(mo.ota_http.OTA_AP_DEFAULT_HOST)


class OtaRollbackEspErrorPathTests(unittest.TestCase):
    def test_http_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mo.ota_http, "rollback_esp",
            side_effect=mo.ota_http.OtaHttpError("admin session required", status=401, detail="unauthorized"),
        ):
            result = mo.ota_rollback_esp(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("admin session required", result)
        self.assertIn("401", result)


class NoPasswordParameterTests(unittest.TestCase):
    """The retired AP-password HMAC leaves no trace on these tools' own
    signatures -- neither takes a `password` argument, and no
    KILNCTL_AP_PASSWORD lookup happens anywhere in this module any more."""

    def test_ota_rollback_esp_has_no_password_parameter(self):
        import inspect
        sig = inspect.signature(mo.ota_rollback_esp)
        self.assertNotIn("password", sig.parameters)

    def test_sw_reset_esp_has_no_password_parameter(self):
        import inspect
        sig = inspect.signature(mo.sw_reset_esp)
        self.assertNotIn("password", sig.parameters)

    def test_sw_reset_esp_calls_through_with_no_credential(self):
        with unittest.mock.patch.object(
            mo.ota_http, "sw_reset",
            return_value={"ok": True, "detail": "resetting"},
        ) as mock_reset:
            result = mo.sw_reset_esp(confirm=True, host="10.0.0.5")
        mock_reset.assert_called_once_with("10.0.0.5")
        self.assertTrue(result.startswith("ok"))


if __name__ == "__main__":
    unittest.main()
