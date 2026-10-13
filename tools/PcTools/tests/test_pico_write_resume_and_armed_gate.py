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

    def test_ok_false_with_halted_core_still_reported(self):
        # OpenOCD prints Error: for handled errors, so ok=False is the common case.
        _, (ok, out) = self._tcl("Error: x\nKCTL_AFTER core0 halted\n", ok=False)
        self.assertFalse(ok)
        self.assertIn("RESUME FAILED / still halted", out)

    def test_ok_false_write_error_reported(self):
        _, (ok, out) = self._tcl("Error: x\nKCTL_WRITE_ERR bad\nKCTL_AFTER core0 running\n", ok=False)
        self.assertFalse(ok)
        self.assertIn("KCTL_WRITE_ERR", out)

    def test_halt_is_catch_wrapped_and_resume_follows(self):
        tcl, _ = self._tcl()
        self.assertIn("catch {halt}", tcl)
        self.assertLess(tcl.index("catch {halt}"), tcl.index("resume;"))

    def test_leave_halted_skips_resume(self):
        with unittest.mock.patch.object(debug_probe, "_run",
                                        return_value=(True, "KCTL_AFTER core0 halted\n")) as run:
            ok, _ = debug_probe.write_memory("pico", 0x40014004, 5, 32, leave_halted=True)
        self.assertTrue(ok)
        self.assertNotIn("resume;", run.call_args[0][1])

    def test_pico_gpio_write_word_surfaces_halted(self):
        with unittest.mock.patch.object(debug_probe, "_run", return_value=(False, "Error: x\nKCTL_AFTER core0 halted\n")):
            with self.assertRaises(RuntimeError) as cm:
                pico_gpio_probe._write_word("pico", 0x40014004, 5)
        self.assertIn("RESUME FAILED", str(cm.exception))


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


class WriteWrapperAndHaltErrTest(unittest.TestCase):
    """toolfx7 (REVIEW_TOOLS3 M2, L5)."""

    def test_wrapper_has_no_leave_halted_and_never_passes_it(self):
        import inspect
        from kilnctrl import mcp_server_debug as msd
        fn = getattr(msd.debug_write_memory, "fn", msd.debug_write_memory)
        self.assertNotIn("leave_halted", inspect.signature(fn).parameters)
        with unittest.mock.patch.object(msd.debug_probe, "pico_armed_state", return_value=(False, "x")), \
             unittest.mock.patch.object(msd.debug_probe, "write_memory", return_value=(True, "")) as wm, \
             unittest.mock.patch.object(msd, "_write_readback_note", return_value="\nread-back OK (0x5)"):
            msd.debug_write_memory("pico", 0x40014004, 5, 32, confirm=True)
        self.assertNotIn("leave_halted", wm.call_args.kwargs)
        self.assertEqual(wm.call_args.args, ("pico", 0x40014004, 5, 32))

    def test_halt_err_is_reported_as_warning(self):
        with unittest.mock.patch.object(debug_probe, "_run", return_value=(
                False, "Error: halt\nKCTL_HALT_ERR boom\nKCTL_AFTER core0 running\n")):
            ok, out = debug_probe.write_memory("pico", 0x40014004, 5, 32)
        self.assertTrue(ok)
        self.assertIn("KCTL_HALT_ERR", out.split("WARNING:")[1])


if __name__ == "__main__":
    unittest.main()
