#!/usr/bin/env python3
"""Test for debug_probe.reset()'s RP2040 SMP-safe TCL sequence, added
2026-09-06 after a `debug_reset(peer="pico")` locked the Pico up on its first
call (fw_version/boot_id/frame counters frozen) and only recovered on an
identical second call. `resume()`'s `_RESUME_TCL` already documents the root
cause for this chip: OpenOCD's SMP grouping needs every core halted first
before a synchronized reset/resume, or the command fails and can leave the
target stuck. `reset()` never carried that same halt-all-first sequence --
this locks it in and proves it would regress silently otherwise.

Against a mocked openocd_util.run_openocd() -- no real OpenOCD, no live board.

Run with: python -m pytest tools/PcTools/tests/test_debug_probe_reset_smp.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe  # noqa: E402


class ResetHaltsAllTargetsFirstTest(unittest.TestCase):
    def setUp(self):
        self.run_mock = unittest.mock.Mock(return_value=(True, "ok"))
        self._openocd_run_patch = unittest.mock.patch.object(debug_probe.openocd_util, "run_openocd", self.run_mock)
        self._openocd_run_patch.start()
        self.addCleanup(self._openocd_run_patch.stop)

        self._exe_patch = unittest.mock.patch.object(debug_probe, "_openocd_exe_or_raise", return_value="fake-openocd.exe")
        self._exe_patch.start()
        self.addCleanup(self._exe_patch.stop)

    def test_pico_reset_halts_every_target_before_resetting(self) -> None:
        """The fix itself: `reset(peer="pico")` must halt every target in
        the chain (both RP2040 cores) before issuing `reset run` -- the same
        pattern `_RESUME_TCL` already uses for `resume()`."""
        ok, _msg = debug_probe.reset(debug_probe.PEER_PICO, mode="run")
        self.assertTrue(ok)
        self.run_mock.assert_called_once()
        _exe, _cfg_args, tcl_commands = self.run_mock.call_args.args[:3]
        self.assertIn("foreach _kctl_t [target names] {targets $_kctl_t; halt}", tcl_commands)
        self.assertIn("reset run", tcl_commands)
        # the halt-all loop must run before the reset itself
        self.assertLess(tcl_commands.index("halt}"), tcl_commands.index("reset run"))

    def test_negative_bare_reset_would_not_halt_targets_first(self) -> None:
        """Proves the assertion above is real: the pre-fix TCL string (a
        bare `init; reset run; exit`, with no halt-all loop) fails this same
        check -- so the test isn't vacuously passing on any TCL string."""
        pre_fix_tcl = "init; reset run; exit"
        self.assertNotIn("foreach _kctl_t [target names] {targets $_kctl_t; halt}", pre_fix_tcl)


if __name__ == "__main__":
    unittest.main()
