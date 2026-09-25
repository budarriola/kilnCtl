#!/usr/bin/env python3
"""Unit tests for mcp_server_control.control_set_zone_limits() -- the MCP
tool that writes ONLY a zone's max_temp_c/min_temp_c over the GET-merge-POST
/api/zones path, without touching PID gains/control_mode/etc the way
load_config_preset() does. All against mocked zones_http_client/
safety_cfg_http_client/profiles/autotune calls; no real socket, no live
board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_control_zone_limits.py -q
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
from kilnctrl import safety_cfg_http_client  # noqa: E402


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
    }
    z.update(overrides)
    return z


def _snapshot(zones):
    return {"thermo_count": len(zones), "relay_count": len(zones), "zones": zones}


def _idle_exec_status():
    return unittest.mock.Mock(state_name="idle", profile_id=0, name="")


def _idle_autotune_status():
    return unittest.mock.Mock(state=0, state_name="idle", zone=0)


def _commissioning(abs_max_value=200.0, set_=True, reliable=True):
    return {
        "unset_reporting_reliable": reliable,
        "params": [
            {"name": "abs_max_temp_c", "value": abs_max_value, "set": set_},
        ],
    }


class _Base(unittest.TestCase):
    def setUp(self):
        self._patches = [
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
            unittest.mock.patch.object(mc._srv._profiles, "get_exec_status", return_value=_idle_exec_status()),
            unittest.mock.patch.object(mc._srv._autotune, "get_status", return_value=_idle_autotune_status()),
            unittest.mock.patch.object(safety_cfg_http_client, "get_commissioning", return_value=_commissioning()),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)


class DryRunTest(_Base):
    def test_dry_run_never_posts(self):
        before = _snapshot([_zone(0, max_temp_c=36.4)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=False)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()

    def test_no_fields_given_is_an_error(self):
        result = mc.control_set_zone_limits(zone=0, confirm=True)
        self.assertIn("error", result.lower())


class ConfirmGateTest(_Base):
    def test_confirm_not_exactly_true_is_refused_as_dry_run(self):
        before = _snapshot([_zone(0, max_temp_c=36.4)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=1)  # truthy, not True
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()


class RunningRefusalTest(_Base):
    def test_running_profile_refuses_before_touching_zones(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="running", profile_id=3, name="bisque")), \
             unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("running", result)
        get_mock.assert_not_called()

    def test_paused_profile_refuses(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="paused", profile_id=3, name="bisque")):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("refused", result.lower())

    def test_running_autotune_refuses(self):
        with unittest.mock.patch.object(mc._srv._autotune, "get_status",
                                         return_value=unittest.mock.Mock(state=2, state_name="stepping", zone=1)):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("autotune", result.lower())


class ZoneRangeTest(_Base):
    def test_unknown_zone_index_refused(self):
        before = _snapshot([_zone(0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before):
            result = mc.control_set_zone_limits(zone=5, max_temp_c=80.0, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)

    def test_min_must_be_strictly_less_than_max(self):
        before = _snapshot([_zone(0, max_temp_c=80.0, min_temp_c=0.0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before):
            result = mc.control_set_zone_limits(zone=0, min_temp_c=80.0, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("min_temp_c", result)


class AbsMaxRefusalTest(_Base):
    def test_max_above_abs_max_refused(self):
        before = _snapshot([_zone(0, max_temp_c=36.4)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(safety_cfg_http_client, "get_commissioning",
                                         return_value=_commissioning(abs_max_value=100.0)), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_limits(zone=0, max_temp_c=150.0, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("abs_max_temp_c", result)
        post_mock.assert_not_called()

    def test_abs_max_unset_does_not_block(self):
        before = _snapshot([_zone(0, max_temp_c=36.4)])
        after = _snapshot([_zone(0, max_temp_c=500.0)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(safety_cfg_http_client, "get_commissioning",
                                         return_value=_commissioning(set_=False)), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=500.0, confirm=True)
        self.assertIn("ok - zone 0", result)


class HappyPathTest(_Base):
    def test_applies_and_confirms_by_readback(self):
        before = _snapshot([_zone(0, max_temp_c=36.4), _zone(1, max_temp_c=80.0)])
        after = copy.deepcopy(before)
        after["zones"][0]["max_temp_c"] = 80.0
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body") as build_mock, \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok") as post_mock:
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("ok - zone 0", result)
        self.assertIn("max_temp_c=80", result)
        build_mock.assert_called_once_with(before, {"zones": [{"index": 0, "max_temp_c": 80.0}]})
        post_mock.assert_called_once_with("10.0.0.5", "body")

    def test_both_fields_at_once(self):
        before = _snapshot([_zone(0, max_temp_c=36.4, min_temp_c=0.0)])
        after = copy.deepcopy(before)
        after["zones"][0]["max_temp_c"] = 80.0
        after["zones"][0]["min_temp_c"] = 5.0
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, min_temp_c=5.0, confirm=True)
        self.assertIn("ok - zone 0", result)
        self.assertIn("max_temp_c=80", result)
        self.assertIn("min_temp_c=5", result)


class PostRefusalTest(_Base):
    def test_post_not_ok_is_surfaced(self):
        before = _snapshot([_zone(0, max_temp_c=36.4)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="zone max_temp_c out of range"):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)


class ReadBackMismatchTest(_Base):
    def test_value_did_not_land_fails_loud(self):
        before = _snapshot([_zone(0, max_temp_c=36.4)])
        after = copy.deepcopy(before)  # unchanged -- POST silently didn't land
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - zone", result)


class CollateralChangeTest(_Base):
    def test_pid_gain_drift_on_target_zone_fails_loud(self):
        """The target field lands correctly, but an unrelated field on the
        SAME zone also changed -- must still fail, never report ok."""
        before = _snapshot([_zone(0, max_temp_c=36.4, pid_kp=1.0)])
        after = copy.deepcopy(before)
        after["zones"][0]["max_temp_c"] = 80.0
        after["zones"][0]["pid_kp"] = 2.5  # collateral drift -- must be caught
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("pid_kp", result)
        self.assertNotIn("ok - zone", result)

    def test_other_zone_change_fails_loud(self):
        """load_config_preset()'s whole-page regression, caught: a change to
        a completely different zone must fail this tool's read-back check."""
        before = _snapshot([_zone(0, max_temp_c=36.4), _zone(1, control_mode=1)])
        after = copy.deepcopy(before)
        after["zones"][0]["max_temp_c"] = 80.0
        after["zones"][1]["control_mode"] = 0  # collateral -- zone 1 untouched by this call
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value="body"), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_limits(zone=0, max_temp_c=80.0, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("zone 1", result)
        self.assertIn("control_mode", result)


if __name__ == "__main__":
    unittest.main()
