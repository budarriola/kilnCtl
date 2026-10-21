#!/usr/bin/env python3
"""RV-05/06/07: soak wait loops to 35 s with a cap, is guarded; debug_reset orders it after refusals."""
from __future__ import annotations

import os
import re
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "src"))
from kilnctrl import reset_probe  # noqa: E402

SRC = os.path.join(HERE, "..", "src", "kilnctrl")


class Clock:
    def __init__(self, up):
        self.t = 0.0
        self.up = up

    def sleep(self, n):
        self.t += n
        self.up += n

    def status(self, h, timeout=0):
        return {"uptime_s": self.up}


class SoakWait(unittest.TestCase):
    def test_reboot_during_sleep_waits_again(self):
        c = Clock(30.0)
        calls = []

        def status(h, timeout=0):
            calls.append(1)
            if len(calls) == 2:
                c.up = 12.0  # board rebooted mid-sleep
            return {"uptime_s": c.up}
        reset_probe.wait_for_boot_soak(status, "h", clock=lambda: c.t, sleep=c.sleep)
        self.assertGreaterEqual(c.up, 35.0)
        self.assertGreaterEqual(len(calls), 3)

    def test_hard_cap(self):
        slept = []
        t = [0.0]

        def sleep(n):
            slept.append(n)
            t[0] += n
        note = reset_probe.wait_for_boot_soak(lambda h, timeout=0: {"uptime_s": 1.0}, "h",
                                              clock=lambda: t[0], sleep=sleep)
        self.assertLessEqual(sum(slept), 40.0)
        self.assertIn("not reached", note)

    def test_nonfinite_uptime_does_not_raise(self):
        for bad in (float("inf"), float("nan"), "x", None):
            slept = []
            note = reset_probe.wait_for_boot_soak(lambda h, timeout=0, b=bad: {"uptime_s": b}, "h",
                                                  sleep=slept.append)
            self.assertEqual(slept, [])
            self.assertIn("not checked", note)

    def test_sleep_error_is_caught(self):
        def sleep(n):
            raise OSError("boom")
        note = reset_probe.wait_for_boot_soak(lambda h, timeout=0: {"uptime_s": 5}, "h", sleep=sleep)
        self.assertIn("not checked", note)


class DebugSource(unittest.TestCase):
    def setUp(self):
        self.src = open(os.path.join(SRC, "mcp_server_debug.py"), encoding="utf-8").read()

    def test_single_definition(self):
        self.assertEqual(len(re.findall(r"^def _wait_boot_soak\(", self.src, re.M)), 1)

    def test_wait_after_refusals_and_run_only(self):
        call = self.src.index("soak_note = _wait_boot_soak()")
        self.assertGreater(call, self.src.index("allow_dark_rereset is not True:"))
        self.assertIn('wait_for_confirm and mode == "run"', self.src[call - 200:call])


if __name__ == "__main__":
    unittest.main()
