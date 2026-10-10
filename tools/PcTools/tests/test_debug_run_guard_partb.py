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
                self.assertTrue(dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=5, confirm=True).startswith("FAILED"))
            with um.patch.object(debug_probe, "read_memory", return_value=(False, "")):
                self.assertIn("UNVERIFIED", dbg.debug_write_memory(peer="esp", address=0x3FC00000, value=5, confirm=True))


class FailClosedStateTests(unittest.TestCase):
    def _ctx(self, exec_exc=None, state=0, at_state=0):
        def ges(timeout=2.0):
            if exec_exc:
                raise exec_exc
            return _status(state)
        prof = um.patch.object(dbg._srv, "_profiles", types.SimpleNamespace(get_exec_status=ges), create=True)
        at = um.patch.object(dbg._srv, "_autotune", types.SimpleNamespace(
            get_status=lambda: types.SimpleNamespace(state=at_state, state_name="relay_cycling", zone=1)), create=True)
        return prof, at

    def test_unreadable_executor_refuses_halt_and_override_works(self):
        prof, at = self._ctx(exec_exc=OSError("down"))
        with prof, at, um.patch.object(debug_probe, "halt", return_value=(True, "")) as h:
            self.assertIn("could not be read", dbg.debug_halt(peer="esp"))
            h.assert_not_called()
            self.assertEqual(dbg.debug_halt(peer="esp", allow_running=True), "halted esp")

    def test_autotune_running_refuses_halt(self):
        prof, at = self._ctx(at_state=4)
        with prof, at, um.patch.object(debug_probe, "halt", return_value=(True, "")) as h:
            self.assertIn("autotune", dbg.debug_halt(peer="esp"))
            h.assert_not_called()

    def test_step_refused_while_running(self):
        prof, at = self._ctx(state=1)
        with prof, at, um.patch.object(debug_probe, "step", return_value=(True, "")) as s:
            self.assertIn("refusing to step", dbg.debug_step(peer="esp"))
            s.assert_not_called()

    def test_leave_halted_reads_refused_but_plain_reads_not(self):
        prof, at = self._ctx(state=1)
        with prof, at, um.patch.object(debug_probe, "read_memory", return_value=(True, "x")) as r,                 um.patch.object(debug_probe, "read_registers", return_value=(True, "x")) as rr:
            self.assertIn("refusing", dbg.debug_read_memory(peer="esp", address=0, leave_halted=True))
            self.assertIn("refusing", dbg.debug_read_registers(peer="esp", leave_halted=True))
            r.assert_not_called()
            rr.assert_not_called()
            self.assertEqual(dbg.debug_read_memory(peer="esp", address=0), "x")


if __name__ == "__main__":
    unittest.main()
