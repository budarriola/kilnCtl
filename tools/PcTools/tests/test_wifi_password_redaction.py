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

THE FIX: redact_secret_fields() (devices_common.py) walks any dict and
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

import contextlib
import json
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.devices_wifi_uart import UartWifiStatus  # noqa: E402
from kilnctrl.devices_common import redact_secret_fields  # noqa: E402

SENTINEL = "s3cr3t-ApPassw0rd-DO-NOT-LEAK"

# Distinct, unmistakable-if-leaked markers for every OTHER get_board_state()
# section, so a test run against a live serial hub/board can be told apart
# from a hermetic one by inspecting the output -- see
# _patch_all_board_state_sections()'s doc comment.
_INFO_FW_SENTINEL = {"commit": "TEST-SENTINEL-FW"}
_INFO_PINS_SENTINEL = [{"gpio": 999, "function_id": 1}]
_THERMO_SENTINEL = [{"channel": 0, "marker": "TEST-SENTINEL-THERMO"}]
_IO_SENTINEL = {"marker": "TEST-SENTINEL-IO"}
_SAFETY_STATUS_SENTINEL = {"marker": "TEST-SENTINEL-SAFETY-STATUS"}
_SAFETY_LINK_SENTINEL = {"marker": "TEST-SENTINEL-SAFETY-LINK"}
_SAFETY_FW_SENTINEL = {"marker": "TEST-SENTINEL-SAFETY-FW"}
_CONTROL_ZONES_SENTINEL = {"thermo_count": 0, "relay_count": 0, "zones": []}
_PROFILES_SENTINEL = {"marker": "TEST-SENTINEL-PROFILES"}
_AUTOTUNE_SENTINEL = {"marker": "TEST-SENTINEL-AUTOTUNE"}


def _fake_status(ap_password: str = SENTINEL) -> UartWifiStatus:
    return UartWifiStatus(
        mode=1,
        state=2,
        sta_connected=False,
        ssid="home-network",
        ap_ssid="kilnctl-board",
        ap_password=ap_password,
        sta_ip="",
        sta_rssi=-40,
        ap_clients=0,
    )


@contextlib.contextmanager
def _patch_all_board_state_sections(_srv, wifi_status: UartWifiStatus):
    """Patch every client object get_board_state() touches, hermetically.

    2026-09-21 review finding: a first version of this test mocked only
    ``_srv._wifi``, so every OTHER section (fw version, thermo, safety
    status, PID gains, ...) ran against whatever real board/serial hub
    happened to be reachable when the test ran -- silently, since
    ``_snapshot_section()`` catches an unmocked attribute's AttributeError
    and turns it into an ``{"error": ...}`` entry rather than failing loud,
    so a comment claiming "the other sections error out" was untested and
    false whenever a board was actually connected.

    Each mock is built with ``spec=[...]`` naming only the method
    get_board_state() actually calls, so touching any OTHER attribute on
    one of these objects raises ``AttributeError`` immediately (caught by
    ``_snapshot_section()`` into an ``{"error": ...}`` entry for that
    section -- callers must additionally assert the section's value
    against its sentinel, not just its absence of an "error" key, since a
    real board's genuine reading could theoretically also lack one).
    Every method's return value is a unique, unmistakable sentinel dict
    that no real board would ever produce, so a caller can assert the
    section is *exactly* that sentinel: proof this test exercised the
    mock, not live hardware, even with the MCP server/serial hub up.
    """
    with contextlib.ExitStack() as stack:
        def mock_for(spec_methods):
            m = unittest.mock.MagicMock(spec=list(spec_methods))
            return m

        info = mock_for(["get_fw_version", "get_pin_config"])
        info.get_fw_version.return_value = _INFO_FW_SENTINEL
        info.get_pin_config.return_value = _INFO_PINS_SENTINEL

        thermo = mock_for(["read"])
        thermo.read.return_value = _THERMO_SENTINEL

        io = mock_for(["read"])
        io.read.return_value = _IO_SENTINEL

        safety = mock_for(["get_status", "get_link_stats", "get_fw_version"])
        safety.get_status.return_value = _SAFETY_STATUS_SENTINEL
        safety.get_link_stats.return_value = _SAFETY_LINK_SENTINEL
        safety.get_fw_version.return_value = _SAFETY_FW_SENTINEL

        wifi = mock_for(["get_status"])
        wifi.get_status.return_value = wifi_status

        control = mock_for(["get_zones"])
        control.get_zones.return_value = (0, 0, [])

        profiles = mock_for(["get_exec_status"])
        profiles.get_exec_status.return_value = _PROFILES_SENTINEL

        autotune = mock_for(["get_status"])
        autotune.get_status.return_value = _AUTOTUNE_SENTINEL

        for name, mock in (
            ("_info", info), ("_thermo", thermo), ("_io", io), ("_safety", safety),
            ("_wifi", wifi), ("_control", control), ("_profiles", profiles),
            ("_autotune", autotune),
        ):
            stack.enter_context(unittest.mock.patch.object(_srv, name, mock))
        yield


class RedactSecretFieldsUnitTest(unittest.TestCase):
    """Direct unit coverage of the redaction helper itself."""

    def test_password_like_keys_are_redacted(self):
        redacted = redact_secret_fields({
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
        redacted = redact_secret_fields({
            "wifi_status": {"ap_password": SENTINEL, "ap_ssid": "x"},
            "list": [{"password": SENTINEL}, {"other": 1}],
        })
        self.assertEqual(redacted["wifi_status"]["ap_password"], "[set]")
        self.assertEqual(redacted["wifi_status"]["ap_ssid"], "x")
        self.assertEqual(redacted["list"][0]["password"], "[set]")
        self.assertEqual(redacted["list"][1]["other"], 1)

    def test_status_suffix_keys_are_not_redacted(self):
        """A boolean status flag whose name merely contains the secret
        substring (e.g. admin_password_set) must pass through unredacted,
        not collapse to "[set]"/"[unset]" and hide its real value."""
        redacted = redact_secret_fields({
            "ap_password_set": True,
            "admin_password_set": False,
            "ap_password": "x",
            "psk": "",
        })
        self.assertIs(redacted["ap_password_set"], True)
        self.assertIs(redacted["admin_password_set"], False)
        self.assertEqual(redacted["ap_password"], "[set]")
        self.assertEqual(redacted["psk"], "[unset]")


class GetBoardStateRedactionTest(unittest.TestCase):
    """get_board_state() must never leak the sentinel password. Hermetic:
    every section client is patched (see _patch_all_board_state_sections()),
    so this holds regardless of whether a real board/serial hub is
    reachable while the test runs."""

    def _run_get_board_state(self, wifi_status: UartWifiStatus) -> dict:
        from kilnctrl import mcp_server_codec as codec
        from kilnctrl import mcp_server as _srv

        with _patch_all_board_state_sections(_srv, wifi_status):
            output = codec.get_board_state()
        return output, json.loads(output)

    def test_sentinel_never_appears_and_every_section_is_hermetic(self):
        output, parsed = self._run_get_board_state(_fake_status())
        self.assertNotIn(SENTINEL, output)

        # Every non-wifi section must equal its sentinel exactly -- proof
        # get_board_state() read from these mocks and not a live board
        # (whose fw_version/thermo/safety/control readings could never
        # coincidentally match a literal "TEST-SENTINEL-..." marker), and
        # proof no section silently fell back to an {"error": ...} entry
        # from an unmocked/misspelled attribute access.
        self.assertEqual(parsed["fw_version"], _INFO_FW_SENTINEL)
        self.assertEqual(parsed["pin_config"], _INFO_PINS_SENTINEL)
        self.assertEqual(parsed["thermo"], _THERMO_SENTINEL)
        self.assertEqual(parsed["io"], _IO_SENTINEL)
        self.assertEqual(parsed["safety_status"], _SAFETY_STATUS_SENTINEL)
        self.assertEqual(parsed["safety_link_stats"], _SAFETY_LINK_SENTINEL)
        self.assertEqual(parsed["safety_fw_version"], _SAFETY_FW_SENTINEL)
        self.assertEqual(parsed["control_zones"], _CONTROL_ZONES_SENTINEL)
        self.assertEqual(parsed["profiles_exec_status"], _PROFILES_SENTINEL)
        self.assertEqual(parsed["autotune_status"], _AUTOTUNE_SENTINEL)

        # And the one section this test is actually about: redacted, not
        # omitted, not the raw value.
        self.assertEqual(parsed["wifi_status"]["ap_password"], "[set]")

    def test_negative_unset_password_reports_unset(self):
        output, parsed = self._run_get_board_state(_fake_status(ap_password=""))
        self.assertEqual(parsed["wifi_status"]["ap_password"], "[unset]")
        # Hermeticity still holds on this path too.
        self.assertEqual(parsed["fw_version"], _INFO_FW_SENTINEL)


class WifiGetStatusRedactionTest(unittest.TestCase):
    """wifi_get_status() (the human-readable MCP tool) must never render the
    raw password either -- only the "[set]"/"[unset]" marker. This tool
    only ever touches _srv._wifi, so patching that one object is already
    hermetic here."""

    def test_sentinel_never_appears_in_wifi_get_status_output(self):
        from kilnctrl import mcp_server_wifi as wifi_tools
        from kilnctrl import mcp_server as _srv

        wifi = unittest.mock.MagicMock(spec=["get_status"])
        wifi.get_status.return_value = _fake_status()
        with unittest.mock.patch.object(_srv, "_wifi", wifi):
            output = wifi_tools.wifi_get_status()

        self.assertNotIn(SENTINEL, output)
        self.assertIn("ap_password=[set]", output)


if __name__ == "__main__":
    unittest.main()
