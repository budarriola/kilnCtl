"""H2/H3 (REVIEW_MCP_TOOL_GATES_2026-10-10): write_memory must always resume the core;
pico_gpio_set_mode/pico_gpio_write must refuse when ARMED or ARMED unreadable.
Fake OpenOCD runner / fake probe only; no board."""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe, mcp_server_pico_gpio_probe as mp, pico_gpio_probe  # noqa: E402,F401


class WriteMemoryResumeTest(unittest.TestCase):
    def _tcl(self, output="KCTL_AFTER core0 running\nKCTL_AFTER core1 running\n", ok=True):
        with unittest.mock.patch.object(debug_probe, "_run", return_value=(ok, output)) as run:
            res = debug_probe.write_memory("pico", 0x40014004, 5, 32)
        return run.call_args[0][1], res

    def test_script_resumes_after_write_even_if_write_errors(self):
        tcl, (ok, _) = self._tcl()
        self.assertTrue(ok)
        self.assertLess(tcl.index("mww"), tcl.index("resume;"))
        self.assertIn("catch {mww", tcl)  # a write error cannot skip the resume tail

    def test_resume_error_reported_loudly(self):
        _, (ok, out) = self._tcl("KCTL_RESUME_ERR boom\nKCTL_AFTER core0 halted\n")
        self.assertFalse(ok)
        self.assertIn("RESUME FAILED", out)

    def test_still_halted_reported(self):
        _, (ok, out) = self._tcl("KCTL_AFTER core0 halted\n")
        self.assertFalse(ok)
        self.assertIn("HALTED", out)

    def test_write_error_fails(self):
        _, (ok, out) = self._tcl("KCTL_WRITE_ERR bad addr\nKCTL_AFTER core0 running\n")
        self.assertFalse(ok)
        self.assertIn("KCTL_WRITE_ERR", out)


class PicoGpioArmedGateTest(unittest.TestCase):
    def _call(self, fn, armed, *args):
        with unittest.mock.patch.object(mp.debug_probe, "pico_armed_state", return_value=armed), \
             unittest.mock.patch.object(mp.pico_gpio_probe, "set_mode") as sm, \
             unittest.mock.patch.object(mp.pico_gpio_probe, "write") as wr, \
             unittest.mock.patch.object(mp.pico_gpio_probe, "read", return_value=True):
            r = fn(*args, confirm=True)
        return r, sm, wr

    def test_armed_refuses_no_probe_io(self):
        for fn, args in ((mp.pico_gpio_set_mode, (10, "output")), (mp.pico_gpio_write, (10, True))):
            r, sm, wr = self._call(fn, (True, "ARMED"), *args)
            self.assertIn("ARMED", r)
            self.assertTrue(r.startswith("error"), r)
            sm.assert_not_called()
            wr.assert_not_called()

    def test_unreadable_refuses_no_probe_io(self):
        for fn, args in ((mp.pico_gpio_set_mode, (10, "output")), (mp.pico_gpio_write, (10, True))):
            r, sm, wr = self._call(fn, (None, "no elf"), *args)
            self.assertIn("could not confidently determine", r)
            sm.assert_not_called()
            wr.assert_not_called()

    def test_not_armed_proceeds(self):
        r, sm, _ = self._call(mp.pico_gpio_set_mode, (False, "ok"), 10, "output")
        self.assertTrue(r.startswith("ok"), r)
        sm.assert_called_once()

    def test_gpio6_still_refused_by_probe(self):
        with unittest.mock.patch.object(mp.debug_probe, "pico_armed_state", return_value=(False, "ok")):
            r = mp.pico_gpio_write(6, True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)


if __name__ == "__main__":
    unittest.main()
