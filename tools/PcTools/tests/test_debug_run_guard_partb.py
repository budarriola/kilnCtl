"""debug_reset/halt/write_memory refuse while a profile runs; write read-back."""
from __future__ import annotations

import os
import sys
import types
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe, reset_probe  # noqa: E402
from kilnctrl import mcp_server_debug as dbg  # noqa: E402


def _status(state):
    return types.SimpleNamespace(state=state, state_name="RUNNING")


class RunGuardTests(unittest.TestCase):
    def _profiles(self, state):
        return um.patch.object(dbg._srv, "_profiles", types.SimpleNamespace(
            get_exec_status=lambda timeout=2.0: _status(state)), create=True)

    def test_reset_refused_while_running(self):
        with self._profiles(1), um.patch.object(debug_probe, "reset") as r:
            out = dbg.debug_reset(peer="esp")
        self.assertIn("refusing to reset", out)
        r.assert_not_called()

    def test_reset_override_must_be_true(self):
        with self._profiles(1), um.patch.object(debug_probe, "reset") as r:
            out = dbg.debug_reset(peer="esp", allow_running="yes")
        self.assertIn("refusing to reset", out)
        r.assert_not_called()

    def test_halt_refused_then_override(self):
        with self._profiles(2), um.patch.object(debug_probe, "halt", return_value=(True, "")) as h:
            self.assertIn("refusing to halt", dbg.debug_halt(peer="esp"))
            h.assert_not_called()
            self.assertEqual(dbg.debug_halt(peer="esp", allow_running=True), "halted esp")

    def test_pico_reset_not_gated(self):
        with self._profiles(1), um.patch.object(debug_probe, "reset", return_value=(True, "")), \
                um.patch.object(reset_probe, "append_history", return_value=None):
            self.assertIn("OK", dbg.debug_reset(peer="pico"))

    def test_write_esp_refused_while_running(self):
        with self._profiles(1), um.patch.object(debug_probe, "write_memory") as w:
            out = dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=1, confirm=True)
        self.assertIn("refusing", out)
        w.assert_not_called()

    def test_write_readback_ok_and_mismatch(self):
        with self._profiles(0), um.patch.object(debug_probe, "write_memory", return_value=(True, "")):
            with um.patch.object(debug_probe, "read_memory", return_value=(True, "MEMRD 0x3fc00000 0x00000005")):
                self.assertIn("read-back OK", dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=5, confirm=True))
            with um.patch.object(debug_probe, "read_memory", return_value=(True, "MEMRD 0x3fc00000 0x00000006")):
                self.assertIn("WARNING: read-back", dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=5, confirm=True))
            with um.patch.object(debug_probe, "read_memory", return_value=(False, "")):
                self.assertIn("UNVERIFIED", dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=5, confirm=True))


if __name__ == "__main__":
    unittest.main()
