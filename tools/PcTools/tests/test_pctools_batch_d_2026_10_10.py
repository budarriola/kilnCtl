#!/usr/bin/env python3
"""Regression tests for PcTools fix batch D (batch C review LOWs/NITs plus pooled items).

Fake board / fake http only; never touches hardware.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import (  # noqa: E402
    adaptive_tune_http_client as at_http,
    mcp_server,
    mcp_server_adaptive_tune as m_at,
    mcp_server_debug as m_dbg,
    mcp_server_info as m_info,
    mcp_server_pico_gpio_probe as m_pgp,
    mcp_server_ramp_assist as m_ra,
    mcp_server_wifi as m_wifi,
    mcp_server_zones_current_sweep as m_sw,
    ramp_assist_http_client as ra_http,
    safety_cfg_http_client as scc,
    web_auth_setup_http_client as wac,
    zones_current_sweep_http_client as sw_http,
)


def _exec(state):
    s = um.MagicMock(state=state)
    s.state_name = f"state{state}"
    return s


class RunningGuardsFailClosedTests(unittest.TestCase):
    def test_debug_unknown_profile_state_refused(self):
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", return_value=_exec(7)):
            self.assertIsNotNone(m_dbg._esp_profile_running_refusal("halt"))

    def test_debug_program_pico_refused_while_running(self):
        with um.patch.object(m_dbg, "_esp_profile_running_refusal", return_value="error: refusing") as g:
            out = m_dbg.debug_program("pico", confirm=True)
        self.assertEqual(out, "error: refusing")
        g.assert_called_once()

    def test_debug_program_pico_unreadable_esp_proceeds_with_warning(self):
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", side_effect=RuntimeError("link down")), \
             um.patch.object(m_dbg.stale_check, "check_saftyfw_stale", return_value=um.MagicMock(stale=False)), \
             um.patch.object(m_dbg.debug_probe, "program", return_value=(True, "ok")) as prog, \
             um.patch.object(m_dbg, "_archive_flashed_safty_elf", return_value=""):
            out = m_dbg.debug_program("pico", confirm=True)
        prog.assert_called_once()
        self.assertIn("WARNING", out)
        self.assertIn("could not be read", out)
        self.assertIn("programmed pico OK", out)

    def test_debug_program_pico_exec_idle_autotune_unreadable_refused(self):
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", return_value=_exec(0)),              um.patch.object(m_dbg._srv._autotune, "get_status", side_effect=TimeoutError("t")),              um.patch.object(m_dbg.debug_probe, "program") as prog:
            out = m_dbg.debug_program("pico", confirm=True)
        prog.assert_not_called()
        self.assertTrue(out.startswith("error: refusing"), out)
        self.assertIn("autotune state could not be read", out)

    def test_debug_program_pico_confirmed_running_refused_unreadable_does_not_mask(self):
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", return_value=_exec(1)), \
             um.patch.object(m_dbg.debug_probe, "program") as prog:
            out = m_dbg.debug_program("pico", confirm=True)
        prog.assert_not_called()
        self.assertTrue(out.startswith("error: refusing"), out)

    def test_debug_program_pico_confirmed_running_allow_running_overrides(self):
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", return_value=_exec(1)), \
             um.patch.object(m_dbg.stale_check, "check_saftyfw_stale", return_value=um.MagicMock(stale=False)), \
             um.patch.object(m_dbg.debug_probe, "program", return_value=(True, "ok")) as prog, \
             um.patch.object(m_dbg, "_archive_flashed_safty_elf", return_value=""):
            m_dbg.debug_program("pico", confirm=True, allow_running=True)
        prog.assert_called_once()

    def test_other_debug_guards_still_fail_closed_when_unreadable(self):
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", side_effect=RuntimeError("x")):
            self.assertIn("could not be read", m_dbg._esp_profile_running_refusal("halt"))

    def _pico_program(self, exec_state, at_state, **kw):
        at = um.MagicMock(state=at_state)
        at.state_name = f"at{at_state}"
        stale = um.MagicMock(stale=False)
        with um.patch.object(m_dbg._srv._profiles, "get_exec_status", return_value=_exec(exec_state)), \
             um.patch.object(m_dbg._srv._autotune, "get_status", return_value=at), \
             um.patch.object(m_dbg.stale_check, "check_saftyfw_stale", return_value=stale), \
             um.patch.object(m_dbg, "_archive_flashed_safty_elf", return_value=""), \
             um.patch.object(m_dbg.debug_probe, "program", return_value=(True, "ok")) as prog:
            out = m_dbg.debug_program("pico", confirm=True, **kw)
        return out, prog

    def test_debug_program_pico_refused_during_autotune(self):
        out, prog = self._pico_program(0, 2)
        self.assertIn("autotune", out)
        self.assertIn("refusing", out)
        prog.assert_not_called()

    def test_debug_program_pico_allow_running_overrides(self):
        out, prog = self._pico_program(0, 2, allow_running=True)
        self.assertIn("programmed pico OK", out)
        prog.assert_called_once()

    def test_debug_program_pico_idle_passes(self):
        out, prog = self._pico_program(0, 0)
        self.assertIn("programmed pico OK", out)
        prog.assert_called_once()

    def test_wifi_unknown_profile_state_refused(self):
        with um.patch.object(m_wifi._srv._profiles, "get_exec_status", return_value=_exec(9)):
            self.assertIn("refused", m_wifi._wifi_write_refusal(True, "wifi_connect"))

    def test_wifi_refused_during_autotune(self):
        at = um.MagicMock(state=2)
        at.state_name = "running"
        with um.patch.object(m_wifi._srv._profiles, "get_exec_status", return_value=_exec(0)), \
             um.patch.object(m_wifi._srv._autotune, "get_status", return_value=at):
            self.assertIn("autotune", m_wifi._wifi_write_refusal(True, "wifi_connect"))

    def test_wifi_autotune_unreadable_refused(self):
        with um.patch.object(m_wifi._srv._profiles, "get_exec_status", return_value=_exec(0)), \
             um.patch.object(m_wifi._srv._autotune, "get_status", side_effect=RuntimeError("x")):
            self.assertIn("autotune state could not be read", m_wifi._wifi_write_refusal(True, "w"))

    def test_wifi_idle_passes(self):
        at = um.MagicMock(state=0)
        with um.patch.object(m_wifi._srv._profiles, "get_exec_status", return_value=_exec(0)), \
             um.patch.object(m_wifi._srv._autotune, "get_status", return_value=at):
            self.assertIsNone(m_wifi._wifi_write_refusal(True, "w"))


class PicoGpioWriteUnverifiedTests(unittest.TestCase):
    def test_readback_exception_not_ok(self):
        with um.patch.object(m_pgp.debug_probe, "pico_armed_state", return_value=(False, "not armed")), \
             um.patch.object(m_pgp.pico_gpio_probe, "write"), \
             um.patch.object(m_pgp.pico_gpio_probe, "read", side_effect=RuntimeError("x")):
            out = m_pgp.pico_gpio_write(4, True, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn("state UNKNOWN", out)


class PostCommissioningTransportTests(unittest.TestCase):
    def test_oserror_is_state_unknown(self):
        with um.patch.object(scc.http_auth, "urlopen", side_effect=ConnectionResetError("reset")):
            with self.assertRaises(scc.SafetyCfgHttpError) as cm:
                scc.post_commissioning("h", "a=1")
        self.assertIn("state UNKNOWN", str(cm.exception))


class WebAuthUnreachableFlagTests(unittest.TestCase):
    def test_flag_default_false_and_settable(self):
        self.assertFalse(wac.WebAuthSetupHttpError("x").unreachable)
        self.assertTrue(wac.WebAuthSetupHttpError("x", unreachable=True).unreachable)

    def test_urlerror_raises_unreachable(self):
        import urllib.error
        with um.patch.object(wac.http_auth, "urlopen", side_effect=urllib.error.URLError("nope")):
            with self.assertRaises(wac.WebAuthSetupHttpError) as cm:
                wac.get_auth_config("h")
        self.assertTrue(cm.exception.unreachable)


class ReadBackTests(unittest.TestCase):
    def _ra(self, **kw):
        with um.patch.object(m_ra, "_ramp_assist_resolve_host", return_value="h"), \
             um.patch.object(ra_http, "set_enabled", return_value={"ok": True}), \
             um.patch.object(ra_http, "get_enabled", **kw):
            return m_ra.ramp_assist_set_enabled(True, confirm=True)

    def test_ramp_assist_mismatch_failed(self):
        self.assertTrue(self._ra(return_value=False).startswith("FAILED"))

    def test_ramp_assist_unreadable_unknown(self):
        self.assertIn("state UNKNOWN", self._ra(side_effect=RuntimeError("x")))

    def test_ramp_assist_ok_verified(self):
        out = self._ra(return_value=True)
        self.assertTrue(out.startswith("ok"))
        self.assertIn("read back verified", out)

    def _at(self, **kw):
        with um.patch.object(m_at, "_adaptive_tune_resolve_host", return_value="h"), \
             um.patch.object(at_http, "set_enabled", return_value={"ok": True}), \
             um.patch.object(at_http, "get_status", **kw):
            return m_at.adaptive_tune_set_enabled(1, True, confirm=True)

    def test_adaptive_tune_mismatch_failed(self):
        self.assertTrue(self._at(return_value=[um.MagicMock(zone=1, enabled=False)]).startswith("FAILED"))

    def test_adaptive_tune_missing_row_unknown(self):
        self.assertIn("state UNKNOWN", self._at(return_value=[]))

    def test_adaptive_tune_unreadable_unknown(self):
        self.assertIn("state UNKNOWN", self._at(side_effect=RuntimeError("x")))

    def test_adaptive_tune_ok_verified(self):
        out = self._at(return_value=[um.MagicMock(zone=1, enabled=True)])
        self.assertTrue(out.startswith("ok"))

    def _sweep(self, status):
        board = um.MagicMock(reachable=True, crash_unacknowledged=False, readiness_blocked=[], heat_blocked=[])
        with um.patch.object(m_sw, "_zone_sweep_resolve_host", return_value="h"), \
             um.patch.object(m_sw.capability_preflight, "get_board_info", return_value=board), \
             um.patch.object(sw_http, "start", return_value={"ok": True}), \
             um.patch.object(sw_http, "status", **status):
            return m_sw.zone_current_sweep_start(confirm=True)

    def test_sweep_idle_after_start_unverified(self):
        self.assertTrue(self._sweep({"return_value": {"state": "idle"}}).startswith("UNVERIFIED"))

    def test_sweep_running_ok(self):
        self.assertTrue(self._sweep({"return_value": {"state": "running"}}).startswith("ok"))


class BackupImportRelayCyclesKeptTests(unittest.TestCase):
    def test_kept_counters_reported(self):
        from kilnctrl import backup_import_http_client, readiness_http_client
        from kilnctrl import mcp_server_ota
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            f.write("{}")
            path = f.name
        try:
            with um.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="h"), \
                 um.patch.object(readiness_http_client, "get_readiness", return_value={"items": []}), \
                 um.patch.object(backup_import_http_client, "post_import",
                                 return_value=(200, '{"ok":true,"relay_cycles_kept":[1,3]}')):
                out = m_info.backup_import(path, confirm=True)
        finally:
            os.unlink(path)
        self.assertIn("were NOT lowered", out)
        self.assertIn("[1, 3]", out)


class ClearTripLeadTests(unittest.TestCase):
    def _diag(self, reason, mask):
        return um.MagicMock(ever_received=True, trip_reason=reason, trip_mask=mask)

    def test_readback_failure_leads_unverified(self):
        from kilnctrl.mcp_server_safety import SafetyQueryError
        with um.patch.object(mcp_server._safety, "get_diag",
                             side_effect=[self._diag(6, 0x20), SafetyQueryError("x")]),              um.patch.object(mcp_server, "_send", return_value="ok - sent"):
            out = mcp_server.safety_clear_trip()
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn("state UNKNOWN", out)

    def test_still_latched_leads_with_state(self):
        with um.patch.object(mcp_server._safety, "get_diag", return_value=self._diag(6, 0x20)),              um.patch.object(mcp_server, "_send", return_value="ok - sent"),              um.patch("kilnctrl.mcp_server_safety._CLEAR_TRIP_READBACK_POLL_S", 0):
            out = mcp_server.safety_clear_trip(allow_unexpected_mask=True)
        self.assertTrue(out.startswith("STILL LATCHED"), out)


class ProfilesSaveSegmentCompareTests(unittest.TestCase):
    def test_segment_content_mismatch_failed(self):
        import json
        import types
        from kilnctrl import mcp_server_profiles as mp
        bad = types.SimpleNamespace(target_c=900.0, ramp_c_per_hr=50.0, dwell_min=1)
        got = types.SimpleNamespace(name="p", segments=[bad], zone_mask=1)
        res = types.SimpleNamespace(ok=True, id=3, warning_count=0, error="")
        prof = um.MagicMock()
        prof.save.return_value = res
        prof.get.return_value = got
        segs = json.dumps([{"target_c": 100, "ramp_c_per_hr": 50, "dwell_min": 1}])
        from kilnctrl import aux_http_client as ahc, mcp_server_ota
        with um.patch.object(mp._srv, "_profiles", prof, create=True),              um.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="1.2.3.4"),              um.patch.object(ahc, "_get_json", return_value={"on_off_rules": [], "segments": []}):
            out = mp.profiles_save(3, "p", 1, segs)
        self.assertTrue(out.startswith("FAILED"), out)


if __name__ == "__main__":
    unittest.main()
