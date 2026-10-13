#!/usr/bin/env python3
"""Unit tests for mcp_server.expander_reset() -- resets the SX1509 I/O
expander (IO_CMD_SX_RESET over task UART_TASK_ID_IO): software reset via
RegReset, or a hard pulse of ~RESET (GPIO10).

Relevant memory: "debug_reset does not power-cycle external I2C peripherals
... a second debug_reset clears it" (CLAUDE.md) -- this is the one MCP tool
that actually resets the SX1509 itself, so getting its refusal/silent-
success decode wrong would hide exactly the failure mode that note
describes.

No real socket and no live board: IoClient.sx_reset() is mocked directly,
same convention as test_control_set_zone_model.py. The malformed/short-frame
path is exercised against the real wire decoder (devices.parse_io_response),
proving a truncated refusal reply for this write subcommand is rejected
rather than silently read as success.

Run with: python -m pytest tools/PcTools/tests/test_expander_reset_mcp.py -q
"""
from __future__ import annotations

import os
import struct
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl import devices  # noqa: E402
from kilnctrl.devices import OkReason, IoResponseError  # noqa: E402
from kilnctrl.io_expander import IoQueryError  # noqa: E402
from kilnctrl.protocol import IO_CMD_SX_RESET  # noqa: E402


_RUN_GATE = "kilnctrl.mcp_server_control._profile_or_autotune_running_reason"
_gate_patch = unittest.mock.patch(_RUN_GATE, return_value=None)


def setUpModule():
    _gate_patch.start()


def tearDownModule():
    _gate_patch.stop()


class ExpanderWriteGateTests(unittest.TestCase):
    CALLS = [
        lambda **k: mcp_server.expander_write_reg(1, 2, **k),
        lambda **k: mcp_server.expander_set_dir(1, **k),
        lambda **k: mcp_server.expander_set_pullup(1, **k),
        lambda **k: mcp_server.expander_set_opendrain(1, **k),
        lambda **k: mcp_server.expander_set_debounce(1, **k),
        lambda **k: mcp_server.expander_set_int_mask(1, **k),
        lambda **k: mcp_server.expander_led_driver(1, True, **k),
        lambda **k: mcp_server.expander_reset(**k),
    ]

    def test_refused_without_exact_confirm(self):
        with unittest.mock.patch.object(mcp_server._io, "_write_style") as w:
            for c in self.CALLS:
                self.assertTrue(c().startswith("refused"))
                self.assertTrue(c(confirm="yes").startswith("refused"))
            w.assert_not_called()

    def test_refused_mid_run(self):
        with unittest.mock.patch(_RUN_GATE, return_value="a profile is running"):
            for c in self.CALLS:
                out = c(confirm=True)
                self.assertTrue(out.startswith("refused") and "live" in out, out)


class ExpanderResetHappyPathTests(unittest.TestCase):
    def test_silent_success_soft_reset_reports_ok(self):
        with unittest.mock.patch.object(
            mcp_server._io, "sx_reset", return_value=OkReason(ok=True)
        ) as mock_reset:
            result = mcp_server.expander_reset(hard=False, confirm=True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("soft reset", result)
        mock_reset.assert_called_once_with(False)

    def test_hard_reset_reports_hard_in_label(self):
        with unittest.mock.patch.object(
            mcp_server._io, "sx_reset", return_value=OkReason(ok=True)
        ):
            result = mcp_server.expander_reset(hard=True, confirm=True)
        self.assertIn("hard reset", result)


class ExpanderResetRefusalTests(unittest.TestCase):
    def test_safety_refusal_reports_reason(self):
        with unittest.mock.patch.object(
            mcp_server._io, "sx_reset", return_value=OkReason(ok=False, reason="safety"),
        ):
            result = mcp_server.expander_reset(hard=False, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("safety", result)


class ExpanderResetMalformedFrameTests(unittest.TestCase):
    """The optional refusal reply this tool's client waits on is decoded by
    devices.parse_io_response(); prove a truncated SX_RESET refusal reply
    (missing even the ok byte) is rejected rather than decoded as success."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", IO_CMD_SX_RESET)  # subcmd only
        with self.assertRaises(IoResponseError):
            devices.parse_io_response(payload)

    def test_well_formed_refusal_decodes(self):
        reason = b"driver error"
        payload = struct.pack("<BBB", IO_CMD_SX_RESET, 0, len(reason)) + reason
        subcommand, value = devices.parse_io_response(payload)
        self.assertEqual(subcommand, IO_CMD_SX_RESET)
        self.assertIsInstance(value, OkReason)
        self.assertFalse(value.ok)
        self.assertEqual(value.reason, "driver error")


class ExpanderResetErrorPathTests(unittest.TestCase):
    def test_undelivered_request_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            mcp_server._io, "sx_reset", side_effect=IoQueryError("not delivered"),
        ):
            result = mcp_server.expander_reset(hard=False, confirm=True)
        self.assertTrue(result.startswith("error"))
        self.assertIn("not delivered", result)


if __name__ == "__main__":
    unittest.main()
