#!/usr/bin/env python3
"""Regression test for the 2026-09-21 AP-password disclosure fix.

THE BUG: get_board_state() (mcp_server_codec.py) passed the UART
GET_STATUS reply (UartWifiStatus, devices_wifi_uart.py) straight through
dataclasses.asdict() into its JSON snapshot. That reply legitimately
carries the board's own AP Wi-Fi password in plaintext (the UART link's
trust model is physical access, not authentication -- see
uart_bridge_ext_wifi.c's GET_STATUS handler), but nothing downstream of
that decode should ever repeat the value into a tool's rendered output.
A bench agent reported the plaintext password reachable via
get_board_state()'s wifi_status block.

THE FIX: _redact_secret_fields() (devices_common.py) walks any dict and
replaces a value whose key matches /password|psk|passphrase/i with
"[set]"/"[unset]", applied to get_board_state()'s full state dict before
it is serialized. wifi_get_status() (mcp_server_wifi.py) was also changed
to render the same "[set]"/"[unset]" marker instead of omitting the field.

This test feeds a fake UartWifiStatus carrying a sentinel password string
through both tools (via a mocked _srv._wifi) and asserts the sentinel
never appears anywhere in either tool's output -- neither raw nor
JSON-escaped/re-encoded.

Run with: tools\\PcTools\\.venv\\Scripts\\python.exe -m pytest tools/PcTools/tests/test_wifi_password_redaction.py -q
"""
from __future__ import annotations

import json
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.devices_wifi_uart import UartWifiStatus  # noqa: E402
from kilnctrl.devices_common import _redact_secret_fields  # noqa: E402

SENTINEL = "s3cr3t-ApPassw0rd-DO-NOT-LEAK"


def _fake_status() -> UartWifiStatus:
    return UartWifiStatus(
        mode=1,
        state=2,
        sta_connected=False,
        ssid="home-network",
        ap_ssid="kilnctl-board",
        ap_password=SENTINEL,
        sta_ip="",
        sta_rssi=-40,
        ap_clients=0,
    )


class RedactSecretFieldsUnitTest(unittest.TestCase):
    """Direct unit coverage of the redaction helper itself."""

    def test_password_like_keys_are_redacted(self):
        redacted = _redact_secret_fields({
            "ap_password": SENTINEL,
            "web_password": "",
            "psk": "another-secret",
            "passphrase": "yet-another",
            "ssid": "kept-as-is",
        })
        self.assertEqual(redacted["ap_password"], "[set]")
        self.assertEqual(redacted["web_password"], "[unset]")
        self.assertEqual(redacted["psk"], "[set]")
        self.assertEqual(redacted["passphrase"], "[set]")
        self.assertEqual(redacted["ssid"], "kept-as-is")

    def test_recurses_into_nested_dicts_and_lists(self):
        redacted = _redact_secret_fields({
            "wifi_status": {"ap_password": SENTINEL, "ap_ssid": "x"},
            "list": [{"password": SENTINEL}, {"other": 1}],
        })
        self.assertEqual(redacted["wifi_status"]["ap_password"], "[set]")
        self.assertEqual(redacted["wifi_status"]["ap_ssid"], "x")
        self.assertEqual(redacted["list"][0]["password"], "[set]")
        self.assertEqual(redacted["list"][1]["other"], 1)


class GetBoardStateRedactionTest(unittest.TestCase):
    """get_board_state() must never leak the sentinel password, mocking
    _srv._wifi.get_status() to return a fake UartWifiStatus carrying it."""

    def _run_get_board_state(self):
        from kilnctrl import mcp_server_codec as codec
        from kilnctrl import mcp_server as _srv

        with unittest.mock.patch.object(_srv, "_wifi") as mock_wifi:
            mock_wifi.get_status.return_value = _fake_status()
            # Every other section is left unmocked; _snapshot_section()
            # catches whatever AttributeError/etc. results and reports
            # {"error": ...} for that section instead of raising -- only
            # wifi_status needs to be real for this test.
            return codec.get_board_state()

    def test_sentinel_never_appears_in_get_board_state_output(self):
        output = self._run_get_board_state()
        self.assertNotIn(SENTINEL, output)
        parsed = json.loads(output)
        wifi_status = parsed["wifi_status"]
        self.assertNotIsInstance(wifi_status, str)  # got a real section, not an error dict
        self.assertEqual(wifi_status["ap_password"], "[set]")

    def test_negative_unset_password_reports_unset(self):
        from kilnctrl import mcp_server_codec as codec
        from kilnctrl import mcp_server as _srv

        status = _fake_status()
        empty_status = UartWifiStatus(**{**status.__dict__, "ap_password": ""})
        with unittest.mock.patch.object(_srv, "_wifi") as mock_wifi:
            mock_wifi.get_status.return_value = empty_status
            output = codec.get_board_state()
        parsed = json.loads(output)
        self.assertEqual(parsed["wifi_status"]["ap_password"], "[unset]")


class WifiGetStatusRedactionTest(unittest.TestCase):
    """wifi_get_status() (the human-readable MCP tool) must never render the
    raw password either -- only the "[set]"/"[unset]" marker."""

    def test_sentinel_never_appears_in_wifi_get_status_output(self):
        from kilnctrl import mcp_server_wifi as wifi_tools
        from kilnctrl import mcp_server as _srv

        with unittest.mock.patch.object(_srv, "_wifi") as mock_wifi:
            mock_wifi.get_status.return_value = _fake_status()
            output = wifi_tools.wifi_get_status()

        self.assertNotIn(SENTINEL, output)
        self.assertIn("ap_password=[set]", output)


if __name__ == "__main__":
    unittest.main()
