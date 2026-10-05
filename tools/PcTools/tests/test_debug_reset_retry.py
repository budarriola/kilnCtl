"""debug_probe.reset(esp): in-session bounded `reset run` retry (M18 dark board).

The fake OpenOCD replaces debug_probe._run: it records the Tcl and returns the
output a real session printed for a good reset, a stale-halt reset recovered by
one retry, and one that stays dark."""
import unittest
import unittest.mock

from kilnctrl import debug_probe

C0, C1 = "esp32s3.cpu0", "esp32s3.cpu1"


def _state(c0, c1):
    return f"KCTL_STATE {C0} {c0}\nKCTL_STATE {C1} {c1}\n"


GOOD = "KCTL_RESET_ISSUED\n" + _state("running", "running")
RECOVERED = ("KCTL_RESET_ISSUED\n" + _state("halted", "halted") + "KCTL_RETRY 1\n"
             + _state("running", "running"))
DARK = ("KCTL_RESET_ISSUED\n" + _state("halted", "halted") + "KCTL_RETRY 1\n"
        + _state("halted", "halted") + "KCTL_RETRY 2\n" + _state("halted", "halted")
        + f"KCTL_RESUMED {C0}\nKCTL_FINAL {C0} halted\n"
        + f"KCTL_RESUMED {C1}\nKCTL_FINAL {C1} halted\n")


class ResetRetryTest(unittest.TestCase):
    def _reset(self, output, peer="esp", mode="run"):
        with unittest.mock.patch.object(debug_probe, "_run", return_value=(True, output)) as r:
            ok, out = debug_probe.reset(peer, mode)
        return ok, out, r.call_args[0][1]

    def test_good_reset_no_retry(self):
        ok, out, _ = self._reset(GOOD)
        self.assertTrue(ok)
        self.assertEqual(debug_probe.parse_post_reset(out)["retries"], 0)

    def test_recovered_by_retry_is_ok(self):
        ok, out, _ = self._reset(RECOVERED)
        info = debug_probe.parse_post_reset(out)
        self.assertTrue(ok)
        self.assertEqual(info["retries"], 1)
        self.assertFalse(info["dark"])
        self.assertEqual(info["not_running"], {})

    def test_still_dark_after_retries_fails(self):
        ok, out, _ = self._reset(DARK)
        info = debug_probe.parse_post_reset(out)
        self.assertFalse(ok)
        self.assertTrue(info["dark"])
        self.assertEqual(info["retries"], 2)
        self.assertIn("NOT running", out)

    def test_tcl_has_bounded_in_session_retry(self):
        _, _, tcl = self._reset(GOOD)
        self.assertEqual(tcl.count("reset run"), 2)  # first reset + the retry body
        self.assertIn(f"$_kctl_n < {debug_probe._ESP_RESET_RETRIES}", tcl)
        self.assertLess(tcl.index("KCTL_RESET_ISSUED"), tcl.index("KCTL_RETRY"))
        self.assertLess(tcl.index("KCTL_RETRY"), tcl.index("KCTL_RESUMED"))
        self.assertIn("halt", tcl[tcl.index("KCTL_RETRY"):tcl.index("reset run}", tcl.index("KCTL_RETRY"))])
        self.assertTrue(tcl.rstrip().endswith("exit"))

    def test_no_interrupt_mask_leftover(self):
        _, _, tcl = self._reset(GOOD)
        self.assertNotIn("reg ps", tcl)

    def test_retry_bound_is_small(self):
        self.assertTrue(1 <= debug_probe._ESP_RESET_RETRIES <= 3)

    def test_non_esp_and_non_run_modes_have_no_retry(self):
        for peer, mode in (("pico", "run"), ("esp", "halt")):
            with self.subTest(peer=peer, mode=mode):
                _, _, tcl = self._reset("", peer, mode)
                self.assertNotIn("KCTL_RETRY", tcl)


if __name__ == "__main__":
    unittest.main()
