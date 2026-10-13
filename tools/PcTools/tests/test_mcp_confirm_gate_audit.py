#!/usr/bin/env python3
"""Regression tests for docs/audits/MCP_CONFIRM_GATE_AUDIT_2026-10-09.md fixes
F1 (factory_default_then_load_preset), F2 (flash_firmware confirm_erase) and
F6 (load_config_preset): each refuses without a real ``True`` and does no work.

Run with: python -m pytest tools/PcTools/tests/test_mcp_confirm_gate_audit.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl import config_presets  # noqa: E402


class FactoryDefaultGateTests(unittest.TestCase):
    def test_refuses_without_exact_true_and_does_not_touch_the_board(self):
        for bad in (False, "yes", 1, "true"):
            with unittest.mock.patch.object(config_presets, "load_preset_data") as load:
                result = mcp_server.factory_default_then_load_preset("bench_fixture", confirm=bad)
            self.assertTrue(result.startswith("error: refusing"), (bad, result))
            load.assert_not_called()


class LoadConfigPresetGateTests(unittest.TestCase):
    def test_refuses_without_exact_true(self):
        for bad in (False, "yes", 1):
            with unittest.mock.patch.object(config_presets, "apply_preset") as apply:
                result = mcp_server.load_config_preset("bench_fixture", confirm=bad)
            self.assertTrue(result.startswith("refused"), (bad, result))
            apply.assert_not_called()

    def test_refuses_mid_run_and_when_status_unreadable(self):
        with unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                                 return_value="could not read profile exec status (x) -- refusing to guess"),                 unittest.mock.patch.object(config_presets, "apply_preset") as apply:
            result = mcp_server.load_config_preset("bench_fixture", confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        apply.assert_not_called()


class FlashFirmwareEraseGateTests(unittest.TestCase):
    def test_gate_is_exact_true_in_source(self):
        src = open(os.path.join(os.path.dirname(__file__), "..", "src", "kilnctrl",
                                "mcp_server_flash.py"), encoding="utf-8").read()
        self.assertIn("if confirm_erase is not True:", src)
        self.assertNotIn("if not confirm_erase:", src)


if __name__ == "__main__":
    unittest.main()
