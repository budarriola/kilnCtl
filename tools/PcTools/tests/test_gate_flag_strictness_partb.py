"""Gate-flag name recognition and confirm gates on fixture / pico gpio writers."""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcpkit.registry import _is_gate_flag_name  # noqa: E402
from kilnctrl import mcp_server_fixture as fx  # noqa: E402
from kilnctrl import mcp_server_pico_gpio_probe as pg  # noqa: E402


class GateNameTests(unittest.TestCase):
    def test_bench_opt_in_flags_are_gates(self):
        for n in ("attended", "skip_backup", "lcd_stop_heat", "lcd_edit_heat",
                  "ota_allow_heat", "allow_heat", "confirm", "ack_x"):
            self.assertTrue(_is_gate_flag_name(n), n)
        self.assertFalse(_is_gate_flag_name("host"))


class FixtureRelayTests(unittest.TestCase):
    def test_unconfirmed_refused_no_call(self):
        f = um.MagicMock()
        with um.patch.object(fx, "_get_fixture", return_value=f):
            for c in (False, "yes", 1):
                self.assertIn("confirm=True", fx.fixture_set_relay("r", True, confirm=c))
        f.set_relay.assert_not_called()

    def test_readback_mismatch_fails(self):
        f = um.MagicMock()
        f.get_relays.return_value = {"r": False}
        with um.patch.object(fx, "_get_fixture", return_value=f):
            self.assertIn("FAILED", fx.fixture_set_relay("r", True, confirm=True))
        f.get_relays.return_value = {"r": True}
        with um.patch.object(fx, "_get_fixture", return_value=f):
            self.assertTrue(fx.fixture_set_relay("r", True, confirm=True).startswith("ok"))


class PicoGpioConfirmTests(unittest.TestCase):
    def test_unconfirmed_refused(self):
        with um.patch.object(pg.pico_gpio_probe, "write") as w, \
                um.patch.object(pg.pico_gpio_probe, "set_mode") as m:
            self.assertIn("confirm=True", pg.pico_gpio_write(4, True))
            self.assertIn("confirm=True", pg.pico_gpio_set_mode(4, "output", confirm="yes"))
        w.assert_not_called()
        m.assert_not_called()

    def test_write_readback_warning(self):
        with um.patch.object(pg.pico_gpio_probe, "write"), \
                um.patch.object(pg.pico_gpio_probe, "read", return_value=False):
            self.assertIn("WARNING", pg.pico_gpio_write(4, True, confirm=True))


if __name__ == "__main__":
    unittest.main()
