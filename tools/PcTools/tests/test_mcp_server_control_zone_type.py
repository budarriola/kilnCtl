#!/usr/bin/env python3
"""Unit tests for mcp_server_control.control_set_zone_type() -- the MCP tool
that writes ONLY a zone's zone_type over the GET-merge-POST /api/zones path,
without touching PID gains/control_mode/limits/etc the way
load_config_preset() does. Modeled directly on
test_mcp_server_control_zone_limits.py. All against mocked
zones_http_client/profiles/autotune calls; no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_control_zone_type.py -q
"""
from __future__ import annotations

import copy
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_control as mc  # noqa: E402
from kilnctrl import zones_http_client  # noqa: E402


def _zone(index, **overrides):
    z = {
        "index": index,
        "max_temp_c": 80.0,
        "min_temp_c": 0.0,
        "pid_kp": 1.0,
        "pid_ki": 0.1,
        "pid_kd": 0.0,
        "control_mode": 1,
        "relay_mask": 1 << index,
        "max_ramp_c_per_hr": 300.0,
        "zone_type": 0,
    }
    z.update(overrides)
    return z


def _snapshot(zones):
    return {"thermo_count": len(zones), "relay_count": len(zones), "zones": zones}


def _idle_exec_status():
    return unittest.mock.Mock(state_name="idle", profile_id=0, name="")


def _idle_autotune_status():
    return unittest.mock.Mock(state=0, state_name="idle", zone=0)


class _Base(unittest.TestCase):
    def setUp(self):
        self._patches = [
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
            unittest.mock.patch.object(mc._srv._profiles, "get_exec_status", return_value=_idle_exec_status()),
            unittest.mock.patch.object(mc._srv._autotune, "get_status", return_value=_idle_autotune_status()),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)


class ArgValidationTest(_Base):
    def test_bad_zone_type_refused_before_any_io(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            for bad in (2, -1, 3, True, False, "1", 1.0):
                result = mc.control_set_zone_type(zone=0, zone_type=bad, confirm=True)
                self.assertTrue(result.startswith("refused"), (bad, result))
        get_mock.assert_not_called()


class DryRunTest(_Base):
    def test_dry_run_never_posts(self):
        before = _snapshot([_zone(0, zone_type=0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=False)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()


class ConfirmGateTest(_Base):
    def test_confirm_not_exactly_true_is_refused_as_dry_run(self):
        before = _snapshot([_zone(0, zone_type=0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=1)  # truthy, not True
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()


class RunningRefusalTest(_Base):
    def test_running_profile_refuses_before_touching_zones(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="running", profile_id=3, name="bisque")), \
             unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("running", result)
        get_mock.assert_not_called()

    def test_paused_profile_refuses(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="paused", profile_id=3, name="bisque")):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("refused", result.lower())

    def test_running_autotune_refuses(self):
        with unittest.mock.patch.object(mc._srv._autotune, "get_status",
                                         return_value=unittest.mock.Mock(state=2, state_name="stepping", zone=1)):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("autotune", result.lower())


class ZoneRangeTest(_Base):
    def test_unknown_zone_index_refused(self):
        before = _snapshot([_zone(0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before):
            result = mc.control_set_zone_type(zone=5, zone_type=1, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)


class HappyPathTest(_Base):
    def test_applies_and_confirms_by_readback(self):
        before = _snapshot([_zone(0, zone_type=0), _zone(1, zone_type=1)])
        after = copy.deepcopy(before)
        after["zones"][0]["zone_type"] = 1
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="z0_zone_type=1&z0_pid_kp=1.5&z0_k=0.5&z0_tau=9&z0_coupling_c1=0.2&z0_hystc=3") as build_mock, \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok") as post_mock:
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("ok - zone 0", result)
        self.assertIn("zone_type=1", result)
        self.assertIn("refused at start (HP-02", result)
        build_mock.assert_called_once_with(before, {"zones": [{"index": 0, "zone_type": 1}]})
        post_mock.assert_called_once_with("10.0.0.5", "z0_zone_type=1&z0_pid_kp=1.5")

    def test_set_back_to_heater(self):
        before = _snapshot([_zone(0, zone_type=1)])
        after = copy.deepcopy(before)
        after["zones"][0]["zone_type"] = 0
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_type(zone=0, zone_type=0, confirm=True)
        self.assertIn("ok - zone 0", result)
        self.assertIn("zone_type=0", result)
        self.assertIn("needs a max_temp_c ceiling", result)


class RealPostBodyTest(_Base):
    def test_only_the_zonetype_field_differs_from_a_plain_echo(self):
        """Unmocked build_post_body(): the tool's override must change exactly
        one form field versus echoing the GET snapshot unchanged -- coupling
        cells, PID gains and every other zone's fields go out as read."""
        import urllib.parse
        sys.path.insert(0, os.path.dirname(__file__))
        import test_zones_http_client as tz
        cur = tz._sample_get_response()
        for z in cur["zones"]:
            z["zone_type"] = 0
        cur["zones"][1]["coupling_c0"] = 0.1234
        echo = dict(urllib.parse.parse_qsl(zones_http_client.build_post_body(cur, {"zones": []})))
        body = dict(urllib.parse.parse_qsl(zones_http_client.build_post_body(
            cur, {"zones": [{"index": 1, "zone_type": 1}]})))
        diff = {k: (echo.get(k), body.get(k)) for k in set(echo) | set(body) if echo.get(k) != body.get(k)}
        self.assertEqual(diff, {"z1_zonetype": ("0", "1")})
        self.assertEqual(body["z1_coupling_c0"], "0.1234")


class PostRefusalTest(_Base):
    def test_post_not_ok_is_surfaced(self):
        before = _snapshot([_zone(0, zone_type=0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="zone zone_type out of range"):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)

    def test_system_mode_gate_409_is_surfaced(self):
        before = _snapshot([_zone(0, zone_type=0)])
        err = zones_http_client.ZonesHttpError(
            "POST /api/zones refused: HTTP 409", 409,
            "refused -- a firing or autotune run is active; zone configuration cannot be "
            "changed until it ends")
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", side_effect=err):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("system_mode_gate", result)
        self.assertIn("409", result)


class ReadBackMismatchTest(_Base):
    def test_value_did_not_land_fails_loud(self):
        """Negative test target: with the read-back check removed, this
        would incorrectly report 'ok'. Restore by hand (never git checkout)
        and force a full rebuild/re-run after verifying the failure."""
        before = _snapshot([_zone(0, zone_type=0)])
        after = copy.deepcopy(before)  # unchanged -- POST silently didn't land
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - zone", result)


class CollateralChangeTest(_Base):
    def test_pid_gain_drift_on_target_zone_fails_loud(self):
        before = _snapshot([_zone(0, zone_type=0, pid_kp=1.0)])
        after = copy.deepcopy(before)
        after["zones"][0]["zone_type"] = 1
        after["zones"][0]["pid_kp"] = 2.5  # collateral drift -- must be caught
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("pid_kp", result)
        self.assertNotIn("ok - zone", result)

    def test_other_zone_change_fails_loud(self):
        """load_config_preset()'s whole-page regression, caught: a change to
        a completely different zone must fail this tool's read-back check --
        this is exactly the HP-02 shape (a preset restore silently leaving
        zone_type=1 on a zone this tool never touched)."""
        before = _snapshot([_zone(0, zone_type=0), _zone(1, zone_type=0)])
        after = copy.deepcopy(before)
        after["zones"][0]["zone_type"] = 1
        after["zones"][1]["zone_type"] = 1  # collateral -- zone 1 untouched by this call
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("zone 1", result)
        self.assertIn("zone_type", result)

    def test_top_level_config_change_fails_loud(self):
        before = _snapshot([_zone(0, zone_type=0)])
        before["timing_profiles"] = [{"index": 0, "name": "default", "ramp_lock_band_c": 25.0}]
        after = copy.deepcopy(before)
        after["zones"][0]["zone_type"] = 1
        after["timing_profiles"][0]["ramp_lock_band_c"] = 0.0
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_type(zone=0, zone_type=1, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("timing_profiles", result)


if __name__ == "__main__":
    unittest.main()
