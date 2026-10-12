#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_clear_trip() -- the fire-and-forget
request that asks the safety processor to re-check and clear a latched trip
(SAFETY_CMD_CLEAR_TRIP over _srv._send(), no reply frame at all).

This tool has no reply to decode (safety_clear_trip() is a broadcast, per
its own docstring), so "malformed/short frame" here means a delivery-layer
failure -- the send itself being NACKed/undeliverable -- rather than a
malformed reply payload. mcp_server._send() and mcp_server._link.send() are
mocked directly, same boundary test_safety_ct_cal.py and mcp_server.py's own
_send() docstring describe.

Run with: python -m pytest tools/PcTools/tests/test_safety_clear_trip.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl.serial_link import SendResult  # noqa: E402


class SafetyClearTripHappyPathTests(unittest.TestCase):
    def test_delivered_reports_ok(self):
        with unittest.mock.patch.object(
            mcp_server, "_send", return_value="ok - delivered to the destination task's inbox (ACK)"
        ) as mock_send:
            with unittest.mock.patch.object(mcp_server._safety, "get_diag", return_value=unittest.mock.MagicMock(ever_received=True, trip_reason=0, trip_mask=0)):
                result = mcp_server.safety_clear_trip()
        self.assertTrue(result.startswith("ok"))
        mock_send.assert_called_once()


class SafetyClearTripDeliveryFailureTests(unittest.TestCase):
    """Exercise the real _send() (not a mocked stand-in) against a mocked
    transport, so an undeliverable send is proven to surface as a non-ok
    string instead of being reported as a successful clear."""

    def test_undeliverable_send_is_not_reported_as_ok(self):
        with unittest.mock.patch.object(
                 type(mcp_server._info), "compatible",
                 new_callable=unittest.mock.PropertyMock, return_value=True), \
             unittest.mock.patch.object(
                 mcp_server._link, "send",
                 return_value=unittest.mock.MagicMock(
                     value=SendResult.UNDELIVERABLE,
                     ok=False,
                     describe=lambda: "destination task not registered on the peer (NACK)",
                 ),
             ):
            with unittest.mock.patch.object(mcp_server._safety, "get_diag", return_value=unittest.mock.MagicMock(ever_received=True, trip_reason=0, trip_mask=0)):
                result = mcp_server.safety_clear_trip()
        self.assertFalse(result.startswith("ok"))
        self.assertIn("UNDELIVERABLE", result)


class SafetyClearTripErrorPathTests(unittest.TestCase):
    def test_incompatible_protocol_refuses_before_sending(self):
        with unittest.mock.patch.object(
                 type(mcp_server._info), "compatible",
                 new_callable=unittest.mock.PropertyMock, return_value=False), \
             unittest.mock.patch.object(mcp_server._link, "send") as mock_link_send:
            with unittest.mock.patch.object(mcp_server._safety, "get_diag", return_value=unittest.mock.MagicMock(ever_received=True, trip_reason=0, trip_mask=0)):
                result = mcp_server.safety_clear_trip()
        self.assertTrue(result.startswith("error"))
        self.assertIn("protocol version mismatch", result)
        mock_link_send.assert_not_called()


if __name__ == "__main__":
    unittest.main()
