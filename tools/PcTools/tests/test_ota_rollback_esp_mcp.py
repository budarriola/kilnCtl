#!/usr/bin/env python3
"""Unit tests for mcp_server_ota.ota_rollback_esp() -- the "deliberately
revert the ESP32-S3 to its previous firmware image right now" tool
(POST /api/ota/esp/rollback). This is destructive-adjacent (the board
reboots into different code) and, per its own docstring, has never been
exercised against real hardware -- so the string-formatting/error-path
logic exercised here is the only test coverage this tool has at all.

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
            result = mo.ota_rollback_esp("hunter2", host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        self.assertIn("1.4.2", result)
        self.assertIn("rebooting", result)
        mock_rollback.assert_called_once_with("10.0.0.5", "hunter2")


class OtaRollbackEspRefusalTests(unittest.TestCase):
    def test_no_previous_image_reports_board_reason(self):
        with unittest.mock.patch.object(
            mo.ota_http, "rollback_esp",
            return_value={"ok": False, "reason": "no previous valid image to roll back to"},
        ):
            result = mo.ota_rollback_esp("hunter2", host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("no previous valid image to roll back to", result)


class OtaRollbackEspHostResolutionTests(unittest.TestCase):
    def test_explicit_host_bypasses_wifi_lookup(self):
        with unittest.mock.patch.object(mo._srv, "_wifi") as mock_wifi, \
             unittest.mock.patch.object(
                 mo.ota_http, "rollback_esp", return_value={"ok": True, "version_before": "1.0.0"}
             ) as mock_rollback:
            mo.ota_rollback_esp("pw", host="kilnctl.local")
        mock_wifi.get_status.assert_not_called()
        mock_rollback.assert_called_once_with("kilnctl.local", "pw")

    def test_no_host_falls_back_to_ap_default_when_sta_unreachable(self):
        with unittest.mock.patch.object(
            mo._srv, "_wifi",
            **{"get_status.side_effect": WifiUartQueryError("no link")},
        ), unittest.mock.patch.object(
            mo.ota_http, "rollback_esp", return_value={"ok": True, "version_before": "1.0.0"}
        ) as mock_rollback:
            mo.ota_rollback_esp("pw")
        mock_rollback.assert_called_once_with(mo.ota_http.OTA_AP_DEFAULT_HOST, "pw")


class OtaRollbackEspErrorPathTests(unittest.TestCase):
    def test_http_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mo.ota_http, "rollback_esp",
            side_effect=mo.ota_http.OtaHttpError("wrong password", status=403, detail="forbidden"),
        ):
            result = mo.ota_rollback_esp("wrong", host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("wrong password", result)
        self.assertIn("403", result)


class ApPasswordEnvFallbackTests(unittest.TestCase):
    """Covers the KILNCTL_AP_PASSWORD fallback added to mirror
    mcp_server_flash.py's ap_password handling: an omitted `password`
    argument should fall back to the env var, the resolved secret must
    never appear in the tool's returned text, and a caller with neither
    source gets a clear error naming the env var."""

    def setUp(self):
        self._old = os.environ.pop(mo.KILNCTL_AP_PASSWORD_ENV, None)
        self.addCleanup(self._restore)

    def _restore(self):
        if self._old is None:
            os.environ.pop(mo.KILNCTL_AP_PASSWORD_ENV, None)
        else:
            os.environ[mo.KILNCTL_AP_PASSWORD_ENV] = self._old

    def test_sw_reset_esp_uses_env_var_when_password_omitted(self):
        os.environ[mo.KILNCTL_AP_PASSWORD_ENV] = "s3cr3t-env-pw"
        with unittest.mock.patch.object(
            mo.ota_http, "sw_reset",
            return_value={"ok": True, "detail": "resetting"},
        ) as mock_reset:
            result = mo.sw_reset_esp(confirm=True, host="10.0.0.5")
        mock_reset.assert_called_once_with("10.0.0.5", "s3cr3t-env-pw")
        self.assertTrue(result.startswith("ok"))
        self.assertNotIn("s3cr3t-env-pw", result)

    def test_sw_reset_esp_explicit_password_wins_over_env(self):
        os.environ[mo.KILNCTL_AP_PASSWORD_ENV] = "env-pw"
        with unittest.mock.patch.object(
            mo.ota_http, "sw_reset",
            return_value={"ok": True, "detail": "resetting"},
        ) as mock_reset:
            result = mo.sw_reset_esp("explicit-pw", confirm=True, host="10.0.0.5")
        mock_reset.assert_called_once_with("10.0.0.5", "explicit-pw")
        self.assertNotIn("explicit-pw", result)
        self.assertNotIn("env-pw", result)

    def test_sw_reset_esp_missing_credential_names_env_var(self):
        # KILNCTL_AP_PASSWORD is unset (removed in setUp).
        with unittest.mock.patch.object(mo.ota_http, "sw_reset") as mock_reset:
            result = mo.sw_reset_esp(confirm=True, host="10.0.0.5")
        mock_reset.assert_not_called()
        self.assertTrue(result.startswith("error"))
        self.assertIn(mo.KILNCTL_AP_PASSWORD_ENV, result)

    def test_ota_rollback_esp_uses_env_var_when_password_omitted(self):
        os.environ[mo.KILNCTL_AP_PASSWORD_ENV] = "rollback-env-pw"
        with unittest.mock.patch.object(
            mo.ota_http, "rollback_esp",
            return_value={"ok": True, "version_before": "1.0.0"},
        ) as mock_rollback:
            result = mo.ota_rollback_esp(host="10.0.0.5")
        mock_rollback.assert_called_once_with("10.0.0.5", "rollback-env-pw")
        self.assertNotIn("rollback-env-pw", result)

    def test_ota_rollback_esp_missing_credential_names_env_var(self):
        with unittest.mock.patch.object(mo.ota_http, "rollback_esp") as mock_rollback:
            result = mo.ota_rollback_esp(host="10.0.0.5")
        mock_rollback.assert_not_called()
        self.assertTrue(result.startswith("error"))
        self.assertIn(mo.KILNCTL_AP_PASSWORD_ENV, result)


if __name__ == "__main__":
    unittest.main()
