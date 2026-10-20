#!/usr/bin/env python3
"""BC-01: reset tools wait out the boot-confirm soak; firmware warn derives from it."""
from __future__ import annotations

import os
import re
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "src"))
from kilnctrl import reset_probe  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


class BootSoakWait(unittest.TestCase):
    def test_young_board_waits_to_35s(self):
        slept = []
        note = reset_probe.wait_for_boot_soak(lambda h, timeout=0: {"uptime_s": 12}, "h",
                                              sleep=slept.append)
        self.assertEqual(slept, [23.0])
        self.assertIn("BC-01", note)

    def test_old_board_does_not_wait(self):
        slept = []
        note = reset_probe.wait_for_boot_soak(lambda h, timeout=0: {"uptime_s": 40}, "h",
                                              sleep=slept.append)
        self.assertEqual(slept, [])
        self.assertEqual(note, "")

    def test_unreadable_uptime_does_not_block(self):
        def boom(h, timeout=0):
            raise OSError("down")
        slept = []
        note = reset_probe.wait_for_boot_soak(boom, "h", sleep=slept.append)
        self.assertEqual(slept, [])
        self.assertIn("not checked", note)

    def test_tools_call_the_wait(self):
        for rel in ("mcp_server_ota.py", "mcp_server_debug.py"):
            src = open(os.path.join(ROOT, "tools/PcTools/src/kilnctrl", rel), encoding="utf-8").read()
            self.assertIn("wait_for_confirm: bool = True", src, rel)
            self.assertRegex(src, r"wait_for_boot_soak", rel)

    def test_firmware_warn_derived_from_soak(self):
        src = open(os.path.join(ROOT, "firmware/KilnFW/App/main_network_http.c"), encoding="utf-8").read()
        m = re.search(r"#define MAIN_OTA_CONFIRM_WARN_MS\s+\(BOOT_CONFIRM_SOAK_MS\s*\+\s*(\d+)\)", src)
        self.assertIsNotNone(m, "warn threshold must derive from BOOT_CONFIRM_SOAK_MS")
        self.assertGreaterEqual(int(m.group(1)), 10000)


if __name__ == "__main__":
    unittest.main()
