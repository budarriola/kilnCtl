#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_request_rollback() -- the PC-facing
side of SAFETY_CMD_ROLLBACK (0x17, CommonFW/docs/LINK_PROTOCOL.md sec 4),
and mcp_server.ota_rollback_pico()'s send call.

No real UART/serial connection is used -- this only checks the byte-exact
wire encoding, mirroring test_safety_set_config.py's conventions for the
same shape of command (a fire-and-forget SAFETY subcommand with no reply on
the wire).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl import mcp_server  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_ROLLBACK  # noqa: E402


class SafetyRequestRollbackEncodeTests(unittest.TestCase):
    def test_wire_id_is_0x17(self):
        # Next free id after SET_CONFIG (0x16) -- see uart_task_ids.h/
        # link_frame.h/kilnlink_rollback.h's own comments on how this value
        # was chosen (every SAFETY_CMD_*/LINK_FRAME_* id checked first).
        self.assertEqual(SAFETY_CMD_ROLLBACK, 0x17)

    def test_no_payload_one_byte_frame(self):
        # Same "cmd byte only" shape as PING -- no fields to encode.
        self.assertEqual(devices.safety_request_rollback(), bytes([SAFETY_CMD_ROLLBACK]))

    def test_matches_commonfw_vector(self):
        # firmware/CommonFW/test/test_rollback.c's test_vector(): {0x17}.
        self.assertEqual(devices.safety_request_rollback(), bytes([0x17]))

    def test_return_type_is_bytes(self):
        self.assertIsInstance(devices.safety_request_rollback(), bytes)

    def test_takes_no_arguments(self):
        # Unlike safety_set_config(tc_type), there is nothing for a caller
        # to supply -- every refusal reason lives entirely on the Pico side.
        with self.assertRaises(TypeError):
            devices.safety_request_rollback(1)  # type: ignore[call-arg]


class OtaRollbackPicoToolTests(unittest.TestCase):
    def test_sends_expected_bytes_on_safety_task(self):
        with unittest.mock.patch.object(mcp_server, "_send", return_value="ok") as mock_send:
            result = mcp_server.ota_rollback_pico()
        self.assertEqual(result, "ok")
        mock_send.assert_called_once()
        task_id, payload = mock_send.call_args.args
        self.assertEqual(task_id, mcp_server.UART_TASK_ID_SAFETY)
        self.assertEqual(payload, bytes([SAFETY_CMD_ROLLBACK]))

    def test_takes_no_arguments(self):
        # _tool()'s wrapper catches a bad-call TypeError and reports it as an
        # error string rather than propagating it -- same "rejected: ..."
        # shape every other MCP tool's argument mismatch produces.
        with unittest.mock.patch.object(mcp_server, "_send") as mock_send:
            result = mcp_server.ota_rollback_pico(processor="pico")  # type: ignore[call-arg]
        mock_send.assert_not_called()
        self.assertTrue(result.startswith("error:"))


if __name__ == "__main__":
    unittest.main()
