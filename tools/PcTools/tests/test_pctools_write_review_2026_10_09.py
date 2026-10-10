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


class SafetyClearTripLinkAndPollTests(unittest.TestCase):
    def test_refuses_when_no_diag_ever_received(self):
        d = um.MagicMock(ever_received=False, trip_reason=0, trip_mask=0)
        with um.patch.object(mcp_server._safety, "get_diag", return_value=d),              um.patch.object(mcp_server, "_send") as s:
            out = mcp_server.safety_clear_trip()
        self.assertTrue(out.startswith("refused"), out)
        s.assert_not_called()

    def test_readback_polls_until_cleared(self):
        latched = um.MagicMock(ever_received=True, trip_reason=6, trip_mask=0x20)
        clear = um.MagicMock(ever_received=True, trip_reason=0, trip_mask=0)
        with um.patch.object(mcp_server._safety, "get_diag", side_effect=[latched, latched, latched, clear]),              um.patch.object(mcp_server, "_send", return_value="ok - sent"),              um.patch("kilnctrl.mcp_server_safety.time.sleep"):
            out = mcp_server.safety_clear_trip()
        self.assertIn("cleared", out)
        self.assertNotIn("STILL LATCHED", out)

    def test_still_latched_after_window(self):
        latched = um.MagicMock(ever_received=True, trip_reason=6, trip_mask=0x20)
        with um.patch.object(mcp_server._safety, "get_diag", return_value=latched),              um.patch.object(mcp_server, "_send", return_value="ok - sent"),              um.patch("kilnctrl.mcp_server_safety.time.sleep"),              um.patch("kilnctrl.mcp_server_safety.time.monotonic", side_effect=[0.0, 0.1, 5.0, 5.1, 5.2]):
            out = mcp_server.safety_clear_trip()
        self.assertIn("STILL LATCHED", out)


class ApplyPresetRealPartialTests(unittest.TestCase):
    """Real _apply_preset_stages path: a raise on zone N reports the zones that landed."""

    def _preset(self):
        return {"name": "p", "zones": [
            {"index": i, "pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0} for i in range(3)]}

    def test_raise_on_zone2_reports_zones_0_and_1_and_reads_back(self):
        control = um.MagicMock()
        control.set_zone_pid.side_effect = [True, True, ControlQueryError("uart timeout")]
        g = lambda i: um.MagicMock(index=i, pid_kp=1.0, pid_ki=0.1, pid_kd=0.0)  # noqa: E731
        control.get_zones.return_value = (None, None, [g(0), g(1)])
        with self.assertRaises(ControlQueryError) as cm:
            config_presets.apply_preset(control, self._preset())
        note = cm.exception.preset_partial
        self.assertIn("zone 0 pid=ok", note)
        self.assertIn("zone 1 pid=ok", note)
        self.assertNotIn("nothing had landed", note)
        control.get_zones.assert_called_once()  # PID read-back ran on the landed zones

    def test_readback_mismatch_shown_in_partial(self):
        control = um.MagicMock()
        control.set_zone_pid.side_effect = [True, ControlQueryError("x")]
        bad = um.MagicMock(index=0, pid_kp=9.0, pid_ki=0.1, pid_kd=0.0)
        control.get_zones.return_value = (None, None, [bad])
        with self.assertRaises(ControlQueryError) as cm:
            config_presets.apply_preset(control, self._preset())
        self.assertIn("zone 0 pid=FAILED", cm.exception.preset_partial)

    def test_first_zone_raise_says_nothing_landed(self):
        control = um.MagicMock()
        control.set_zone_pid.side_effect = ControlQueryError("x")
        with self.assertRaises(ControlQueryError) as cm:
            config_presets.apply_preset(control, self._preset())
        self.assertIn("nothing had landed", cm.exception.preset_partial)




class PresetKeepKeysTests(unittest.TestCase):
    """LOW-6: only keys whose VALUE build_post_body() takes from the preset are kept."""

    def test_get_spelled_model_and_coupling_cells_not_kept(self):
        preset = {"zones": [{"index": 0, "model_k_dc": 1.5, "model_tau_s": 99.0,
                              "model_dead_time_s": 3.0, "coupling_c1": 0.25,
                              "pid_kp": 2.0}]}
        keep = zones_http_client._preset_named_omit_preserved_keys(preset)
        for k in ("z0_k", "z0_tau", "z0_deadtime", "z0_coupling_c1"):
            self.assertNotIn(k, keep)

    def test_coupling_coeff_row_kept_except_diagonal(self):
        preset = {"zones": [{"index": 1, "coupling_coeff": [0.1, 0.0, 0.3]}]}
        keep = zones_http_client._preset_named_omit_preserved_keys(preset)
        self.assertEqual(keep, {"z1_coupling_c0", "z1_coupling_c2"})



class UiRunScriptPresetErrorTests(unittest.TestCase):
    SCRIPT = {"backend": "lcd", "preset": "x"}

    def test_preset_error_class_reported_with_partial(self):
        from kilnctrl import zones_http_client as z
        exc = z.ZonesHttpError("boom")
        exc.preset_partial = "PID written: zone 0 pid=ok"
        with um.patch.object(ui_test_runner, "run_ui_script", side_effect=exc), \
             um.patch.object(ui_test_runner, "load_ui_script", return_value=self.SCRIPT), \
             um.patch(RUN_GATE, return_value=None):
            out = mcp_server.ui_run_script("s", apply_preset=True, confirm=True)
        self.assertTrue(out.startswith("error"), out)
        self.assertIn("zone 0 pid=ok", out)
        self.assertIn("PARTIALLY", out)

    def test_skipped_preset_is_stated(self):
        with um.patch.object(ui_test_runner, "run_ui_script", return_value={"ok": True}), \
             um.patch.object(ui_test_runner, "load_ui_script", return_value=self.SCRIPT):
            out = mcp_server.ui_run_script("s")
        self.assertIn("preset_skipped", out)
        self.assertIn("apply_preset=False", out)


class Low10MiscTests(unittest.TestCase):
    def test_thermo_write_reg_readback_mismatch_warns(self):
        back = um.Mock(values=[0x55])
        with um.patch(RUN_GATE, return_value=None), \
             um.patch.object(mcp_server._thermo, "write_reg", return_value=um.Mock(ok=True, reason="")), \
             um.patch.object(mcp_server._thermo, "read_reg", return_value=back):
            out = mcp_server.thermo_write_reg(0, 1, 0xAA, confirm=True)
        self.assertTrue(out.startswith("warning"), out)
        self.assertIn("0x55", out)

    def test_thermo_write_reg_readback_match_ok(self):
        back = um.Mock(values=[0xAA])
        with um.patch(RUN_GATE, return_value=None), \
             um.patch.object(mcp_server._thermo, "write_reg", return_value=um.Mock(ok=True, reason="")), \
             um.patch.object(mcp_server._thermo, "read_reg", return_value=back):
            out = mcp_server.thermo_write_reg(0, 1, 0xAA, confirm=True)
        self.assertTrue(out.startswith("ok"), out)

    def test_thermo_write_reg_readback_exception_fails(self):
        from kilnctrl.thermo import ThermoQueryError
        with um.patch(RUN_GATE, return_value=None),              um.patch.object(mcp_server._thermo, "write_reg", return_value=um.Mock(ok=True, reason="")),              um.patch.object(mcp_server._thermo, "read_reg", side_effect=ThermoQueryError("boom")):
            out = mcp_server.thermo_write_reg(0, 1, 0xAA, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertNotIn("ok -", out)

    def test_thermo_write_reg_empty_readback_fails(self):
        back = um.Mock(values=[], data=[])
        with um.patch(RUN_GATE, return_value=None),              um.patch.object(mcp_server._thermo, "write_reg", return_value=um.Mock(ok=True, reason="")),              um.patch.object(mcp_server._thermo, "read_reg", return_value=back):
            out = mcp_server.thermo_write_reg(0, 1, 0xAA, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_relay_io_hits_fail_closed_without_segments(self):
        def gj(host, path, timeout):
            if path == "/api/profiles":
                return [{"id": 3, "builtin": False}]
            return {"name": "x"}  # no segments array
        with um.patch.object(ahc, "_get_json", side_effect=gj):
            with self.assertRaises(ahc.AuxHttpError):
                ahc.get_stored_relay_io_hits("h", 4)

    def test_relay_io_hits_finds_segment(self):
        def gj(host, path, timeout):
            if path == "/api/profiles":
                return [{"id": 3, "builtin": False}, {"id": 200, "builtin": True}]
            return {"segments": [{"seg_kind": 0}, {"seg_kind": 1, "io_target": 4}]}
        with um.patch.object(ahc, "_get_json", side_effect=gj):
            self.assertEqual(ahc.get_stored_relay_io_hits("h", 4), {3: [2]})

    def test_bench_aux_rule_5xx_is_unknown_state_not_refusal(self):
        from kilnctrl import mcp_server_aux as ma, mcp_server_ota, profile_edit_http_client as pehc
        aux = {"relays": [{"relay": 4, "enabled": True, "conflicted": False, "tc_zone": 0}]}
        exc = pehc.ProfileEditHttpError("boom", status=500)
        with um.patch.object(ma, "_running_reason", return_value=None), \
             um.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="h"), \
             um.patch.object(ahc, "get_aux_outputs", return_value=aux), \
             um.patch.object(ahc, "_get_json", return_value=[]), \
             um.patch.object(pehc, "post_profile", side_effect=exc):
            out = ma.profile_save_bench_aux_rule(30.0, 28.0, confirm=True)
        self.assertTrue(out.startswith("error"), out)
        self.assertIn("UNKNOWN", out)

    def test_load_config_preset_partial_apply_line(self):
        from kilnctrl import mcp_server_config_presets as mcp_
        res = um.Mock(all_ok=False)
        res.describe.return_value = "zone 1 failed"
        with um.patch(RUN_GATE, return_value=None), \
             um.patch.object(config_presets, "load_preset_data", return_value={}), \
             um.patch.object(config_presets, "apply_preset", return_value=res):
            out = mcp_.load_config_preset("x", confirm=True)
        self.assertIn("FAILED (partial apply)", out)


if __name__ == "__main__":
    unittest.main()


class SafetyRunningGateAllowListTest(unittest.TestCase):
    """_profile_or_autotune_running treats only idle/done/faulted (profile) and
    0/5/6 (autotune) as not running; unknown states refuse (review 2026-10-10 MED)."""

    def _gate(self, prof_name, at_state):
        import types
        from kilnctrl import mcp_server_safety as ms
        prof = types.SimpleNamespace(state_name=prof_name, state=99, profile_id=1, name="p")
        at = types.SimpleNamespace(state=at_state, state_name="x", zone=0)
        srv = types.SimpleNamespace(
            _profiles=types.SimpleNamespace(get_exec_status=lambda: prof),
            _autotune=types.SimpleNamespace(get_status=lambda: at))
        with unittest.mock.patch.object(ms, "_srv", srv):
            return ms._profile_or_autotune_running()

    def test_idle_states_pass(self):
        for n in ("idle", "done", "faulted"):
            self.assertIsNone(self._gate(n, 0))
        self.assertIsNone(self._gate("idle", 5))
        self.assertIsNone(self._gate("idle", 6))

    def test_unknown_profile_state_refuses(self):
        self.assertIsNotNone(self._gate("unknown(7)", 0))
        self.assertIsNotNone(self._gate("running", 0))

    def test_unknown_autotune_state_refuses(self):
        self.assertIsNotNone(self._gate("idle", 9))
        self.assertIsNotNone(self._gate("idle", 2))
