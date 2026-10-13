"""flash_firmware() confirm gate and live board-state refusal (tool-review
finding, 2026-10-09): a flash resets the main board, so it needs exactly
confirm=True and must refuse mid-firing / mid-autotune."""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

import test_flash_board_pinning as pin  # noqa: E402
from kilnctrl import mcp_server_flash as mf  # noqa: E402


class FlashFirmwareConfirmGateTest(unittest.TestCase):
    def setUp(self):
        pin.FlashFirmwareAdapterPinningTest.setUp(self)
        p = unittest.mock.patch.object(mf.serial_link, "list_ports", return_value=[pin._MAIN_BOARD_JTAG])
        p.start()
        self.addCleanup(p.stop)

    def _state(self, hazards=(), unreadable=()):
        p = unittest.mock.patch.object(
            mf, "_recovery_board_state_refusals",
            return_value=(list(hazards), list(unreadable), []))
        p.start()
        self.addCleanup(p.stop)

    def test_default_refuses_without_confirm(self):
        self._state()
        self.assertTrue(mf.flash_firmware(verify=False).startswith("error:"))
        self.run_mock.assert_not_called()

    def test_truthy_non_true_confirm_refused(self):
        self._state()
        for bad in (1, "true", "yes"):
            self.assertTrue(mf.flash_firmware(verify=False, confirm=bad).startswith("error:"))
        self.run_mock.assert_not_called()

    def test_confirm_true_flashes_on_idle_board(self):
        self._state()
        self.assertIn("flashed and verified OK", mf.flash_firmware(verify=False, confirm=True))

    def test_running_profile_hazard_refuses_even_with_override(self):
        self._state(hazards=["a profile is running or paused"])
        r = mf.flash_firmware(verify=False, confirm=True, allow_unreadable_board_state=True)
        self.assertTrue(r.startswith("error:"))
        self.assertIn("hazard", r)
        self.run_mock.assert_not_called()

    def test_unreadable_state_refuses_unless_overridden(self):
        self._state(unreadable=["autotune state could not be read"])
        r = mf.flash_firmware(verify=False, confirm=True)
        self.assertTrue(r.startswith("error:"))
        self.run_mock.assert_not_called()
        self.assertIn("flashed and verified OK",
                      mf.flash_firmware(verify=False, confirm=True, allow_unreadable_board_state=True))

    def test_state_read_waives_link_down(self):
        seen = {}

        def fake(host, allow_link_down=False):
            seen["l"] = allow_link_down
            return [], [], []
        with unittest.mock.patch.object(mf, "_recovery_board_state_refusals", fake):
            mf.flash_firmware(verify=False, confirm=True)
        self.assertTrue(seen["l"])


if __name__ == "__main__":
    unittest.main()
