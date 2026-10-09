#!/usr/bin/env python3
"""Unit tests for mcp_server.control_set_zone_model() -- writes a zone's
feedforward thermal model (steady-state gain, time constant, dead time) over
the CONTROL task (CONTROL_CMD_SET_ZONE_MODEL).

No real socket and no live board: ControlClient.set_zone_model() is mocked
directly, same convention as test_control_set_zone_pid.py mocking
ControlClient.set_zone_pid(). The malformed/short-frame path is exercised
against the real wire decoder (devices_control.parse_control_response) so a
truncated SET_ZONE_MODEL reply cannot be silently decoded as success.

Run with: python -m pytest tools/PcTools/tests/test_control_set_zone_model.py -q
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
from kilnctrl.devices import OkReason, ControlResponseError  # noqa: E402
from kilnctrl.control import ControlQueryError  # noqa: E402
from kilnctrl.protocol import CONTROL_CMD_SET_ZONE_MODEL  # noqa: E402


def _idle():
    return unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                               return_value=None)


def _http(k=50.0, tau=300.0, dt=15.0):
    return unittest.mock.patch(
        "kilnctrl.mcp_server_control.zones_http_client.get_zones",
        return_value={"zones": [{"index": 1, "model_k_dc": k, "model_tau_s": tau,
                                 "model_dead_time_s": dt}]})


def _host():
    return unittest.mock.patch("kilnctrl.mcp_server_control._control_resolve_host", return_value="h")


class ControlSetZoneModelHappyPathTests(unittest.TestCase):
    def test_success_reports_ok_with_zone_after_readback(self):
        with _idle(), _host(), _http(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_model", return_value=OkReason(ok=True)
        ) as mock_set:
            result = mcp_server.control_set_zone_model(1, 50.0, 300.0, 15.0, confirm=True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("zone 1", result)
        mock_set.assert_called_once_with(1, 50.0, 300.0, 15.0)

    def test_refused_reports_reason(self):
        with _idle(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_model",
            return_value=OkReason(ok=False, reason="zone index out of range"),
        ):
            result = mcp_server.control_set_zone_model(9, 50.0, 300.0, 15.0, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("zone index out of range", result)

    def test_unconfirmed_and_truthy_non_bool_refuse_without_writing(self):
        for bad in (False, "yes", 1):
            with unittest.mock.patch.object(mcp_server._control, "set_zone_model") as mock_set:
                result = mcp_server.control_set_zone_model(1, 50.0, 300.0, 15.0, confirm=bad)
            self.assertTrue(result.startswith("refused"), bad)
            mock_set.assert_not_called()

    def test_mid_run_refuses_without_writing(self):
        with unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                                 return_value="autotune is currently 'settling'"),                 unittest.mock.patch.object(mcp_server._control, "set_zone_model") as mock_set:
            result = mcp_server.control_set_zone_model(1, 50.0, 300.0, 15.0, confirm=True)
        self.assertTrue(result.startswith("refused"))
        mock_set.assert_not_called()

    def test_readback_mismatch_fails_loud(self):
        with _idle(), _host(), _http(tau=100.0), unittest.mock.patch.object(
            mcp_server._control, "set_zone_model", return_value=OkReason(ok=True)
        ):
            result = mcp_server.control_set_zone_model(1, 50.0, 300.0, 15.0, confirm=True)
        self.assertTrue(result.startswith("FAILED"))
        self.assertIn("model_tau_s", result)


class ControlSetZoneModelMalformedFrameTests(unittest.TestCase):
    """The reply this tool's client waits on is decoded by
    devices.parse_control_response(); prove a truncated SET_ZONE_MODEL reply
    (missing even the ok byte) is rejected rather than decoded as success."""

    def test_short_reply_missing_ok_byte_is_rejected(self):
        payload = struct.pack("<B", CONTROL_CMD_SET_ZONE_MODEL)  # subcmd only
        with self.assertRaises(ControlResponseError):
            devices.parse_control_response(payload)

    def test_well_formed_ok_reply_decodes_true(self):
        payload = struct.pack("<BB", CONTROL_CMD_SET_ZONE_MODEL, 1)
        subcommand, value = devices.parse_control_response(payload)
        self.assertEqual(subcommand, CONTROL_CMD_SET_ZONE_MODEL)
        self.assertTrue(bool(value))


class ControlSetZoneModelErrorPathTests(unittest.TestCase):
    def test_query_error_surfaces_as_error_string_not_exception(self):
        with _idle(), unittest.mock.patch.object(
            mcp_server._control, "set_zone_model",
            side_effect=ControlQueryError("timed out"),
        ):
            result = mcp_server.control_set_zone_model(0, 50.0, 300.0, 15.0, confirm=True)
        self.assertTrue(result.startswith("error"))
        self.assertIn("timed out", result)


if __name__ == "__main__":
    unittest.main()
