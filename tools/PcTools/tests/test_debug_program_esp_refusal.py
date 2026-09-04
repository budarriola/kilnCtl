#!/usr/bin/env python3
"""Unit tests for debug_program()'s peer="esp" refusal (mcp_server_debug.py).

History: debug_program(peer="esp") reliably fails flash-bank detection/
verify on this board (confirmed repeatedly) -- flash_firmware() is the
sanctioned, working path. Before this change debug_program still accepted
peer="esp" and failed downstream after wasted OpenOCD time. It must now
refuse IMMEDIATELY, before touching OpenOCD at all, and peer="pico" must be
completely unaffected.

All against mocked debug_probe/stale_check -- no real OpenOCD session, no
live board.

Run with: python -m pytest tools/PcTools/tests/test_debug_program_esp_refusal.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_debug as md  # noqa: E402
from kilnctrl import debug_probe  # noqa: E402


class DebugProgramEspRefusalTest(unittest.TestCase):
    def test_esp_is_refused_immediately_without_confirm(self):
        with unittest.mock.patch.object(debug_probe, "program") as program_mock:
            result = md.debug_program(peer="esp")
        self.assertIn("error", result.lower())
        self.assertIn("flash_firmware", result)
        self.assertIn("flash-bank detection", result)
        program_mock.assert_not_called()

    def test_esp_is_refused_even_with_confirm_true(self):
        """NEGATIVE-PROOF-BY-CONSTRUCTION: the refusal must fire before the
        confirm=True gate is even consulted -- proves this isn't just the
        pre-existing "no confirm" refusal wearing esp's message."""
        with unittest.mock.patch.object(debug_probe, "program") as program_mock:
            result = md.debug_program(peer="esp", confirm=True)
        self.assertIn("error", result.lower())
        self.assertIn("flash_firmware", result)
        program_mock.assert_not_called()

    def test_esp_refusal_never_touches_openocd(self):
        """Refusal must return before any OpenOCD interaction -- patch
        debug_probe.program (the only OpenOCD entry point debug_program()
        calls) and confirm it is never invoked for any esp call shape."""
        with unittest.mock.patch.object(debug_probe, "program") as program_mock:
            md.debug_program(peer="esp", confirm=True, allow_stale=True)
            md.debug_program(peer="esp", elf_path="/some/other.elf", confirm=True)
        program_mock.assert_not_called()

    def test_pico_behaviour_is_unchanged_confirm_required(self):
        """peer="pico" must still hit the ordinary confirm=True gate (not the
        esp refusal) when confirm is omitted."""
        with unittest.mock.patch.object(debug_probe, "program") as program_mock:
            result = md.debug_program(peer="pico")
        self.assertIn("confirm=True", result)
        program_mock.assert_not_called()

    def test_pico_still_programs_when_confirmed(self):
        """peer="pico" must reach debug_probe.program() exactly as before --
        the esp refusal must not have collaterally blocked the pico path."""
        program_mock = unittest.mock.Mock(return_value=(True, "ok"))
        stale_result = unittest.mock.Mock(stale=False, reason="")
        with unittest.mock.patch.object(debug_probe, "program", program_mock), \
                unittest.mock.patch.object(md.stale_check, "check_saftyfw_stale", return_value=stale_result), \
                unittest.mock.patch.object(debug_probe, "_safty_fw_root", return_value="/fake/safty"):
            result = md.debug_program(peer="pico", confirm=True)
        self.assertIn("programmed pico OK", result)
        program_mock.assert_called_once_with("pico", None)


if __name__ == "__main__":
    unittest.main()
