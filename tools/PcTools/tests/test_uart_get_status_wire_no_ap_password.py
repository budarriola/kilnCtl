#!/usr/bin/env python3
"""Regression test for the 2026-09-21 owner decision: the UART GET_STATUS
reply must no longer carry the board's AP Wi-Fi password on the wire at all.

THE BUG (prior state): uart_bridge_ext_wifi.c's wifi_build_status() packed
wifi_prov_get_ap_password() -- the board's real AP Wi-Fi password -- verbatim
into the GET_STATUS reply's length-prefixed ap_password field. That UART link
has no authentication concept (physical-access-gated by design; see the
file's own banner comment), so anyone with a USB cable could read the
password in the clear. The 2026-09-21 PC-side fix (redact_secret_fields(),
devices_common.py; test_wifi_password_redaction.py) only stopped *tools*
from repeating an already-received value -- it did nothing about the wire
itself.

THE FIX: uart_bridge_ext_wifi.c's wifi_build_status() (line ~43) now packs a
presence marker ("[set]" / "") instead of the real password. The wire LAYOUT
is unchanged -- it is still the same length-prefixed ASCII string
uart_bridge_ext_put_lstring() always produced -- so devices_wifi_uart.py's
parse_wifi_uart_response() and every existing consumer (mcp_server_wifi.py's
wifi_get_status(), which already only checked truthiness) keep working
unmodified. No protocol version bump.

Two things are checked here:

1. A source-level assertion that uart_bridge_ext_wifi.c's wifi_build_status()
   no longer feeds the real getter's return value directly into
   uart_bridge_ext_put_lstring() for the ap_password field. This is the part
   that actually proves the firmware fix: uart_bridge_ext_wifi.c cannot be
   host-compiled (it pulls MAX31856.h/kiln_io.h/safety_link.h/screen_idle.h/
   the ESP-IDF UART protocol stack -- no host-test infrastructure exists for
   any file in this split; see IMPLEMENTER.md's "handlers are target-build
   only" note), so a source-pattern check is the only assertion available
   short of a full on-target capture, which this task is expressly forbidden
   from doing (do not contact the board).

2. A wire-decode test proving the FIXED wire shape survives
   parse_wifi_uart_response() intact and a "[set]"/"" marker is what
   mcp_server_wifi.py's caller actually sees -- i.e. that the fix's chosen
   wire content is compatible with the existing decode/consumer path, not
   just that it compiles.

NEGATIVE TEST (run by hand, not automated here -- see hand-back notes):
revert uart_bridge_ext_wifi.c's line 43 to
`o = uart_bridge_ext_put_lstring(out, BRIDGE_REPLY_MAX, o, wifi_prov_get_ap_password());`
and re-run test_source_never_emits_real_password_directly -- it fails. Then
restore by hand and force a full target rebuild
(check_00_kilnfw_target_build.ps1) to confirm the source, not a stale
binary, is what was measured.

Run with:
tools\\PcTools\\.venv\\Scripts\\python.exe -m pytest tools/PcTools/tests/test_uart_get_status_wire_no_ap_password.py -q
"""
from __future__ import annotations

import os
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.devices_wifi_uart import parse_wifi_uart_response  # noqa: E402
from kilnctrl.protocol import WIFI_CMD_GET_STATUS  # noqa: E402

_REPO_ROOT = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..")
)
_FW_SOURCE = os.path.join(
    _REPO_ROOT, "firmware", "KilnFW", "App", "drivers", "bridge",
    "uart_bridge_ext_wifi.c",
)


def _read_wifi_build_status_body() -> str:
    with open(_FW_SOURCE, "r", encoding="utf-8") as f:
        text = f.read()
    m = re.search(
        r"static size_t wifi_build_status\(uint8_t \*out\)\s*\{(.*?)\n\}",
        text, re.DOTALL,
    )
    if not m:
        raise AssertionError(
            "could not locate wifi_build_status() in uart_bridge_ext_wifi.c -- "
            "source shape changed; update this test's regex"
        )
    return m.group(1)


def _pack_lstr8(s: str) -> bytes:
    data = s.encode("ascii")
    return struct.pack("<B", len(data)) + data


class SourceNeverEmitsRealPasswordDirectly(unittest.TestCase):
    def test_source_never_emits_real_password_directly(self):
        body = _read_wifi_build_status_body()
        # The real getter must never be passed straight to the reply
        # builder as the string argument -- that is exactly the prior bug.
        # It may still appear (e.g. in a truthiness check), just not as
        # put_lstring(..., wifi_prov_get_ap_password()).
        leak_pattern = re.compile(
            r"uart_bridge_ext_put_lstring\([^;]*,\s*wifi_prov_get_ap_password\(\)\s*\)",
            re.DOTALL,
        )
        self.assertIsNone(
            leak_pattern.search(body),
            "wifi_build_status() still passes the real AP password straight "
            "into the wire reply -- this is the 2026-09-21 disclosure bug",
        )
        # And the fix's own marker must actually be present, so a change
        # that merely deletes the field (rather than replacing its content)
        # doesn't pass this test vacuously.
        self.assertIn(
            '"[set]"', body,
            "expected the '[set]' presence marker to replace the real "
            "password in wifi_build_status()",
        )


class FixedWireShapeDecodesCleanly(unittest.TestCase):
    """Confirms the FIXED wire content (marker instead of real secret)
    round-trips through the real PC-side decoder unchanged in layout,
    and that a real secret used only to derive the marker never appears
    in the decoded result."""

    def _build_status_frame(self, ap_password_field: str) -> bytes:
        # Mirrors wifi_build_status()'s exact field order/types:
        # subcmd, mode, state, connected, ssid(lstr), ap_ssid(lstr),
        # ap_password(lstr), sta_ip(lstr), rssi(int8), ap_clients(uint8).
        out = struct.pack("<BBBB", WIFI_CMD_GET_STATUS, 1, 2, 0)
        out += _pack_lstr8("home-network")
        out += _pack_lstr8("kilnctl-board")
        out += _pack_lstr8(ap_password_field)
        out += _pack_lstr8("")
        out += struct.pack("<bB", -40, 0)
        return out

    def test_set_marker_decodes_and_never_carries_the_real_secret(self):
        real_secret = "s3cr3t-ApPassw0rd-DO-NOT-LEAK"  # what the board never sends now
        # The firmware only ever emits "[set]" or "" post-fix -- build the
        # frame the way the fixed firmware actually would (marker derived
        # from whether a real password is configured, never the value
        # itself reaching the wire).
        marker = "[set]" if real_secret else ""
        frame = self._build_status_frame(marker)
        subcmd, status = parse_wifi_uart_response(frame)
        self.assertEqual(subcmd, WIFI_CMD_GET_STATUS)
        self.assertEqual(status.ap_password, "[set]")
        self.assertNotEqual(status.ap_password, real_secret)
        self.assertNotIn(real_secret, status.ap_password)

    def test_unset_marker_decodes_to_empty(self):
        frame = self._build_status_frame("")
        _, status = parse_wifi_uart_response(frame)
        self.assertEqual(status.ap_password, "")


if __name__ == "__main__":
    unittest.main()
