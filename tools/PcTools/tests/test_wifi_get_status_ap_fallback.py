#!/usr/bin/env python3
"""Unit tests for wifi_get_status()'s AP-fallback reporting (be7bcad4's
`ap_pending_teardown` field) and state-name rendering.

wifi_get_status() reads Wi-Fi state over the UART wire protocol (task 11),
which never carries `ap_pending_teardown` -- only the HTTP GET /status route
(wifi_provision_http.c) does. These tests mock both _srv._wifi (UART) and
urllib.request.urlopen (HTTP) so no real board or socket is involved.

Run with: tools\\PcTools\\.venv\\Scripts\\python.exe -m pytest tools/PcTools/tests/test_wifi_get_status_ap_fallback.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.devices_wifi_uart import UartWifiStatus  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _fake_uart_status(state: int = 3) -> UartWifiStatus:
    return UartWifiStatus(
        mode=0,
        state=state,
        sta_connected=True,
        ssid="home-network",
        ap_ssid="kilnctl-board",
        ap_password="",
        sta_ip="10.0.0.5",
        sta_rssi=-40,
        ap_clients=0,
    )


class StateNameTest(unittest.TestCase):
    """UartWifiStatus.state_name -- the raw wire byte must never be the
    only thing printed (task's "print state by name, not just the numeric
    enum" requirement)."""

    def test_known_states_map_to_names(self):
        names = {0: "ap", 1: "unprovisioned", 2: "connecting", 3: "connected", 4: "reconnecting"}
        for value, name in names.items():
            self.assertEqual(_fake_uart_status(state=value).state_name, name)

    def test_unknown_state_does_not_raise(self):
        self.assertIn("unknown", _fake_uart_status(state=99).state_name)

    def test_wifi_get_status_prints_state_name_not_raw_int(self):
        from kilnctrl import mcp_server_wifi as wifi_tools
        from kilnctrl import mcp_server as _srv

        wifi = unittest.mock.MagicMock(spec=["get_status"])
        wifi.get_status.return_value = _fake_uart_status(state=3)
        with unittest.mock.patch.object(_srv, "_wifi", wifi):
            output = wifi_tools.wifi_get_status()
        self.assertIn("state=connected", output)
        self.assertNotIn("state=3 ", output)


class ApPendingTeardownTest(unittest.TestCase):
    """wifi_get_status(host=...)'s best-effort HTTP supplement."""

    def _run(self, host):
        from kilnctrl import mcp_server_wifi as wifi_tools
        from kilnctrl import mcp_server as _srv

        wifi = unittest.mock.MagicMock(spec=["get_status"])
        wifi.get_status.return_value = _fake_uart_status()
        with unittest.mock.patch.object(_srv, "_wifi", wifi):
            return wifi_tools.wifi_get_status(host=host)

    def test_no_host_given_reports_unknown_without_a_network_call(self):
        # NEGATIVE-shaped: proves the no-host path never touches urllib at
        # all (an unmocked urlopen call here would raise/hang against a
        # real socket) -- this is also the case every existing caller of
        # wifi_get_status() with no `host` argument exercises.
        with unittest.mock.patch("urllib.request.urlopen") as mock_urlopen:
            output = self._run(host=None)
        mock_urlopen.assert_not_called()
        self.assertIn("ap_pending_teardown=unknown", output)

    def test_field_present_true(self):
        body = json.dumps({"mode": "home", "state": "connected", "ap_pending_teardown": True}).encode()
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            output = self._run(host="10.0.0.5")
        self.assertIn("ap_pending_teardown=True", output)

    def test_field_present_false(self):
        body = json.dumps({"mode": "home", "state": "connected", "ap_pending_teardown": False}).encode()
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            output = self._run(host="10.0.0.5")
        self.assertIn("ap_pending_teardown=False", output)

    def test_field_absent_older_firmware(self):
        """NEGATIVE TEST: an older-firmware /status response (pre-be7bcad4,
        no ap_pending_teardown key at all) must read as explicitly unknown,
        never as a silent False -- which would misreport a board that is
        actually deferring its AP teardown as having none in flight."""
        body = json.dumps({"mode": "home", "state": "connected"}).encode()
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            output = self._run(host="10.0.0.5")
        self.assertIn("ap_pending_teardown=unknown (older firmware)", output)

    def test_unreachable_host_reports_unknown_not_raised(self):
        with unittest.mock.patch("urllib.request.urlopen", side_effect=OSError("connection refused")):
            output = self._run(host="10.0.0.5")
        self.assertIn("ap_pending_teardown=unknown (unreachable over HTTP)", output)
        self.assertFalse(output.startswith("error"))


if __name__ == "__main__":
    unittest.main()
