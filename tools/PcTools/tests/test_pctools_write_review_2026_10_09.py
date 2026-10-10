#!/usr/bin/env python3
"""Regression tests for the 2026-10-09 PcTools write-tool review (part A).

Fake board / fake http only. Run: python -m pytest tools/PcTools/tests/test_pctools_write_review_2026_10_09.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock as um
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import (aux_http_client as ahc, config_presets, mcp_server,  # noqa: E402
                      mcp_server_control, ui_test_runner, zones_http_client)
from kilnctrl.control import ControlQueryError  # noqa: E402

RUN_GATE = "kilnctrl.mcp_server_control._profile_or_autotune_running_reason"


class UiRunScriptGateTests(unittest.TestCase):
    def test_default_does_not_apply_preset(self):
        with um.patch.object(ui_test_runner, "run_ui_script", return_value={"ok": True}) as r, \
             um.patch.object(ui_test_runner, "load_ui_script", return_value={"backend": "lcd", "preset": "x"}):
            mcp_server.ui_run_script("s")
        self.assertIs(r.call_args.kwargs["apply_preset"], False)

    def test_apply_preset_needs_confirm_true(self):
        with um.patch.object(ui_test_runner, "run_ui_script") as r, \
             um.patch.object(ui_test_runner, "load_ui_script", return_value={"backend": "lcd", "preset": "x"}):
            out = mcp_server.ui_run_script("s", apply_preset=True, confirm=False)
        self.assertTrue(out.startswith("refused"), out)
        r.assert_not_called()

    def test_apply_preset_refused_mid_run(self):
        with um.patch.object(ui_test_runner, "run_ui_script") as r, \
             um.patch.object(ui_test_runner, "load_ui_script", return_value={"backend": "lcd", "preset": "x"}), \
             um.patch(RUN_GATE, return_value="a profile is currently running"):
            out = mcp_server.ui_run_script("s", apply_preset=True, confirm=True)
        self.assertTrue(out.startswith("refused"), out)
        r.assert_not_called()


class ZoneStripTests(unittest.TestCase):
    def test_regex_covers_hystc_and_coilpower(self):
        for k in ("z0_hystc", "z2_coilpower", "z1_k", "z1_coupling_c2"):
            self.assertTrue(zones_http_client.ZONE_OMIT_PRESERVED_KEY_RE.match(k), k)
        self.assertFalse(zones_http_client.ZONE_OMIT_PRESERVED_KEY_RE.match("z0_kp"))
        self.assertIs(mcp_server_control._ZONE_OMIT_PRESERVED_KEY_RE, zones_http_client.ZONE_OMIT_PRESERVED_KEY_RE)

    def test_strip_keeps_named_keys_only(self):
        body = "z0_hystc=1&z0_kp=2&z0_coilpower=3&z0_k=4"
        out = urllib.parse.parse_qs(zones_http_client.strip_omit_preserved(body, {"z0_k"}))
        self.assertEqual(sorted(out), ["z0_k", "z0_kp"])


class PostTimeoutTests(unittest.TestCase):
    def test_zones_post_timeout_says_state_unknown(self):
        err = urllib.error.URLError(TimeoutError("timed out"))
        with um.patch.object(zones_http_client.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(zones_http_client.ZonesHttpError) as cm:
                zones_http_client.post_zones("h", "a=1")
        self.assertIn("UNKNOWN", str(cm.exception))

    def test_aux_post_timeout_says_state_unknown(self):
        with um.patch.object(ahc.http_auth, "urlopen", side_effect=TimeoutError("t")):
            with self.assertRaises(ahc.AuxHttpError) as cm:
                ahc.post_move_zone_to_aux("h", 0)
        self.assertIn("UNKNOWN", str(cm.exception))


class ApplyPresetPartialTests(unittest.TestCase):
    def test_exception_carries_partial_note(self):
        with um.patch.object(config_presets, "_apply_preset_stages", side_effect=ControlQueryError("boom")):
            with self.assertRaises(ControlQueryError) as cm:
                config_presets.apply_preset(object(), {})
        self.assertTrue(hasattr(cm.exception, "preset_partial"))


class ThermoGateTests(unittest.TestCase):
    def _each(self):
        t = mcp_server
        return [
            lambda **k: t.thermo_config_channel(0, **k),
            lambda **k: t.thermo_set_thresholds(0, 1000.0, 0.0, 80, -20, **k),
            lambda **k: t.thermo_set_cj_offset(0, 1.0, **k),
            lambda **k: t.thermo_clear_faults(0, **k),
            lambda **k: t.thermo_write_reg(0, 1, 2, **k),
        ]

    def test_refuse_without_confirm(self):
        with um.patch.object(mcp_server._thermo, "config_channel") as w:
            for call in self._each():
                self.assertTrue(call().startswith("refused"))
                self.assertTrue(call(confirm="yes").startswith("refused"))
            w.assert_not_called()

    def test_refuse_mid_run(self):
        with um.patch(RUN_GATE, return_value="a profile is currently running"):
            for call in self._each():
                out = call(confirm=True)
                self.assertTrue(out.startswith("refused") and "live" in out, out)


if __name__ == "__main__":
    unittest.main()


class SafetyClearTripPrecheckTests(unittest.TestCase):
    def _diag(self, reason, mask):
        return um.MagicMock(ever_received=True, trip_reason=reason, trip_mask=mask)

    def test_mask_mismatch_refused_without_override(self):
        with um.patch.object(mcp_server._safety, "get_diag", return_value=self._diag(6, 0x0040)), \
             um.patch.object(mcp_server, "_send") as s:
            out = mcp_server.safety_clear_trip()
        self.assertTrue(out.startswith("refused"), out)
        s.assert_not_called()

    def test_expected_mask_sends_and_reports_readback(self):
        with um.patch.object(mcp_server._safety, "get_diag",
                             side_effect=[self._diag(6, 0x0020), self._diag(0, 0)]), \
             um.patch.object(mcp_server, "_send", return_value="ok - sent") as s:
            out = mcp_server.safety_clear_trip()
        s.assert_called_once()
        self.assertIn("cleared", out)

    def test_deassert_fault_needs_confirm(self):
        with um.patch.object(mcp_server._safety, "set_fault_out") as w:
            self.assertTrue(mcp_server.safety_set_fault_out(False).startswith("refused"))
        w.assert_not_called()
