#!/usr/bin/env python3
"""Unit tests for kilnctrl.mcp_server_safety.safety_get_unset_commissioning_params()
-- the READ-ONLY tool naming which safety_cfg_store params make up
GET /api/readiness's "safety_commissioned" item ("N of M applicable safety
parameters still have no value"), which counts them but never names them.

No real socket and no live board: safety_cfg_http_client.get_commissioning()
is mocked directly, same convention as test_safety_rate_guard.py.

Run with: python -m pytest tools/PcTools/tests/test_safety_unset_commissioning_params.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl import safety_cfg_http_client as sc  # noqa: E402


def _payload(params, reliable=True):
    return {
        "link_up": True, "commissioned": False, "unset_reporting_reliable": reliable,
        "stale": False, "cached_config_crc": 1, "live_config_crc": 1,
        "params": params,
    }


class SafetyGetUnsetCommissioningParamsTest(unittest.TestCase):
    def test_reports_no_applicable_params_unset(self):
        payload = _payload([
            {"id": 0x0104, "name": "abs_max_temp_c", "type": "f32", "set": True, "value": 900.0},
        ])
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=payload):
            result = mcp_server.safety_get_unset_commissioning_params()
        self.assertIn("no applicable", result)

    def test_names_unset_applicable_params_by_id_and_name(self):
        payload = _payload([
            {"id": 0x0104, "name": "abs_max_temp_c", "type": "f32", "set": False},
            {"id": 0x031A, "name": "i_normal_a[0]", "type": "f32", "set": False},
        ])
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=payload):
            result = mcp_server.safety_get_unset_commissioning_params()
        self.assertIn("2 applicable", result)
        self.assertIn("0x0104", result)
        self.assertIn("abs_max_temp_c", result)
        self.assertIn("0x031A", result)
        self.assertIn("i_normal_a[0]", result)

    def test_ct_channel_map_excluded_when_summed_topology(self):
        """The task's own live premise: ct_topology committed summed excludes
        ct_channel_map[0..2] from the count regardless of ct_installed."""
        payload = _payload([
            {"id": 0x0109, "name": "ct_installed", "type": "u8", "set": True, "value": 1},
            {"id": 0x031F, "name": "ct_topology", "type": "u8", "set": True, "value": 1},
            {"id": 0x0106, "name": "ct_channel_map[0]", "type": "u8", "set": False},
            {"id": 0x0107, "name": "ct_channel_map[1]", "type": "u8", "set": False},
            {"id": 0x0108, "name": "ct_channel_map[2]", "type": "u8", "set": False},
        ])
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=payload):
            result = mcp_server.safety_get_unset_commissioning_params()
        self.assertIn("no applicable", result)

    def test_unreliable_reporting_is_flagged_and_still_lists_params(self):
        payload = _payload([
            {"id": 0x0104, "name": "abs_max_temp_c", "type": "f32", "set": True, "value": 900.0},
        ], reliable=False)
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=payload):
            result = mcp_server.safety_get_unset_commissioning_params()
        self.assertIn("unset_reporting_reliable=false", result)
        self.assertIn("abs_max_temp_c", result)

    def test_http_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            sc, "get_commissioning", side_effect=sc.SafetyCfgHttpError("unreachable")
        ):
            result = mcp_server.safety_get_unset_commissioning_params()
        self.assertTrue(result.startswith("error"))


if __name__ == "__main__":
    unittest.main()
