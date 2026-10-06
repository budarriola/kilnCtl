#!/usr/bin/env python3
"""Unit tests for mcp_server_control.control_set_zone_coupling() -- the MCP
tool that writes ONE coupling-matrix cell over the GET-merge-POST /api/zones
path, without touching PID gains/control_mode/limits/zone_type/model/other
coupling cells the way load_config_preset() does. Modeled directly on
test_mcp_server_control_zone_type.py. All against mocked
zones_http_client/profiles/autotune calls; no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_control_zone_coupling.py -q
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


def _zone(index, n=3, **overrides):
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
    z.update({f"coupling_c{j}": (0.0 if j == index else 5.0) for j in range(n)})
    z.update(overrides)
    return z


def _snapshot(zones):
    return {"thermo_count": len(zones), "relay_count": len(zones), "zones": zones}


def _fake_build(cur, preset):
    """Stand-in for build_post_body(): emits the override row's cells plus a
    required field (kp) and an omit-preserved one (k), so the tool's
    stripping step has something real to act on."""
    z = preset["zones"][0]
    i = z["index"]
    cells = "&".join(f"z{i}_coupling_c{j}={c}" for j, c in enumerate(z["coupling_coeff"]))
    return f"z{i}_kp=1.0&z{i}_k=3.0&{cells}"


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
    def test_diagonal_refused_before_any_io(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            result = mc.control_set_zone_coupling(zone=1, from_zone=1, coeff=10.0, confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("diagonal", result)
        get_mock.assert_not_called()

    def test_bad_coeff_refused_before_any_io(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            for bad in (float("nan"), float("inf"), -1.0, 100.01, True, False, "1"):
                result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=bad, confirm=True)
                self.assertTrue(result.startswith("refused"), (bad, result))
        get_mock.assert_not_called()

    def test_boundary_values_accepted_as_valid(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        after = copy.deepcopy(before)
        after["zones"][0]["coupling_c1"] = 100.0
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=100.0, confirm=True)
        self.assertIn("ok - zone 0", result)


class DryRunTest(_Base):
    def test_dry_run_never_posts(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=False)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()


class ConfirmGateTest(_Base):
    def test_confirm_not_exactly_true_is_refused_as_dry_run(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "post_zones") as post_mock:
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=1)  # truthy, not True
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()


class RunningRefusalTest(_Base):
    def test_running_profile_refuses_before_touching_zones(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="running", profile_id=3, name="bisque")), \
             unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("running", result)
        get_mock.assert_not_called()

    def test_paused_profile_refuses(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="paused", profile_id=3, name="bisque")):
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=True)
        self.assertIn("refused", result.lower())

    def test_running_autotune_refuses(self):
        with unittest.mock.patch.object(mc._srv._autotune, "get_status",
                                         return_value=unittest.mock.Mock(state=2, state_name="stepping", zone=1)):
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("autotune", result.lower())


class ZoneRangeTest(_Base):
    def test_unknown_zone_index_refused(self):
        before = _snapshot([_zone(0), _zone(1)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before):
            result = mc.control_set_zone_coupling(zone=5, from_zone=1, coeff=24.52, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)

    def test_unknown_from_zone_index_refused(self):
        before = _snapshot([_zone(0), _zone(1)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before):
            result = mc.control_set_zone_coupling(zone=0, from_zone=5, coeff=24.52, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)


class HappyPathTest(_Base):
    def test_applies_and_confirms_by_readback(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        after = copy.deepcopy(before)
        after["zones"][0]["coupling_c2"] = 24.52
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build) as build_mock, \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok") as post_mock:
            result = mc.control_set_zone_coupling(zone=0, from_zone=2, coeff=24.52, confirm=True)
        self.assertIn("ok - zone 0", result)
        self.assertIn("coupling_c2=24.52", result)
        build_mock.assert_called_once_with(
            before, {"zones": [{"index": 0, "coupling_coeff": [0.0, 5.0, 24.52]}]})
        post_mock.assert_called_once_with("10.0.0.5", "z0_kp=1.0&z0_coupling_c2=24.52")

    def test_restores_all_four_bench_cells(self):
        """The actual HP-02-adjacent restoration this tool was built for:
        z0[2]=24.52, z1[2]=28.69, z2[0]=8.08, z2[1]=10.81 -- one call per
        cell, each independently confirmed by read-back."""
        cells = [(0, 2, 24.52), (1, 2, 28.69), (2, 0, 8.08), (2, 1, 10.81)]
        for zone, from_zone, coeff in cells:
            before = _snapshot([_zone(0), _zone(1), _zone(2)])
            after = copy.deepcopy(before)
            after["zones"][zone][f"coupling_c{from_zone}"] = coeff
            with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
                 unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
                 unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
                result = mc.control_set_zone_coupling(zone=zone, from_zone=from_zone, coeff=coeff, confirm=True)
            self.assertIn(f"ok - zone {zone}", result, (zone, from_zone, coeff, result))


class RealPostBodyTest(_Base):
    def test_only_the_coupling_cell_differs_from_a_plain_echo(self):
        """Unmocked build_post_body(): the tool's override must change exactly
        one form field versus echoing the GET snapshot unchanged -- every
        other coupling cell, PID gains, and every other zone's fields go out
        as read."""
        import urllib.parse
        sys.path.insert(0, os.path.dirname(__file__))
        import test_zones_http_client as tz
        cur = tz._sample_get_response()
        cur["zones"][0]["coupling_c1"] = 27.32
        cur["zones"][0]["coupling_c2"] = 21.72
        echo = dict(urllib.parse.parse_qsl(zones_http_client.build_post_body(cur, {"zones": []})))
        n = cur["thermo_count"]
        coeffs = [cur["zones"][0].get(f"coupling_c{j}") for j in range(n)]
        coeffs[2] = 24.52
        body = dict(urllib.parse.parse_qsl(zones_http_client.build_post_body(
            cur, {"zones": [{"index": 0, "coupling_coeff": coeffs}]})))
        diff = {k: (echo.get(k), body.get(k)) for k in set(echo) | set(body) if echo.get(k) != body.get(k)}
        self.assertEqual(diff, {"z0_coupling_c2": (repr(21.72), repr(24.52))})
        self.assertEqual(body["z0_coupling_c1"], repr(27.32))
        self.assertEqual(body["z0_coupling_c0"], repr(0.0))


class PostRefusalTest(_Base):
    def test_post_not_ok_is_surfaced(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="zone coupling_coeff out of range"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("out of range", result)

    def test_system_mode_gate_409_is_surfaced(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        err = zones_http_client.ZonesHttpError(
            "POST /api/zones refused: HTTP 409", 409,
            "refused -- a firing or autotune run is active; zone configuration cannot be "
            "changed until it ends")
        with unittest.mock.patch.object(zones_http_client, "get_zones", return_value=before), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", side_effect=err):
            result = mc.control_set_zone_coupling(zone=0, from_zone=1, coeff=24.52, confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("system_mode_gate", result)
        self.assertIn("409", result)


class ReadBackMismatchTest(_Base):
    def test_value_did_not_land_fails_loud(self):
        """Negative test target: with the read-back check removed, this
        would incorrectly report 'ok'. Restore by hand (never git checkout)
        and force a full rebuild/re-run after verifying the failure."""
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        after = copy.deepcopy(before)  # unchanged -- POST silently didn't land
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=2, coeff=24.52, confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - zone", result)


class CollateralChangeTest(_Base):
    def test_pid_gain_drift_on_target_zone_fails_loud(self):
        before = _snapshot([_zone(0, pid_kp=1.0), _zone(1), _zone(2)])
        after = copy.deepcopy(before)
        after["zones"][0]["coupling_c2"] = 24.52
        after["zones"][0]["pid_kp"] = 2.5  # collateral drift -- must be caught
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=2, coeff=24.52, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("pid_kp", result)
        self.assertNotIn("ok - zone", result)

    def test_other_coupling_cell_change_fails_loud(self):
        """The exact class this tool must not reintroduce: a write that
        lands the target cell correctly but also perturbs a SIBLING cell in
        the same row -- must fail this tool's read-back check."""
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        after = copy.deepcopy(before)
        after["zones"][0]["coupling_c2"] = 24.52
        after["zones"][0]["coupling_c1"] = 9.99  # collateral -- untouched sibling cell drifted
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=2, coeff=24.52, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("coupling_c1", result)

    def test_other_zone_change_fails_loud(self):
        """load_config_preset()'s whole-page regression, caught: a change to
        a completely different zone must fail this tool's read-back check."""
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        after = copy.deepcopy(before)
        after["zones"][0]["coupling_c2"] = 24.52
        after["zones"][1]["coupling_c2"] = 99.0  # collateral -- zone 1 untouched by this call
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=2, coeff=24.52, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("zone 1", result)

    def test_top_level_config_change_fails_loud(self):
        before = _snapshot([_zone(0), _zone(1), _zone(2)])
        before["timing_profiles"] = [{"index": 0, "name": "default", "ramp_lock_band_c": 25.0}]
        after = copy.deepcopy(before)
        after["zones"][0]["coupling_c2"] = 24.52
        after["timing_profiles"][0]["ramp_lock_band_c"] = 0.0
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[before, after]), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", side_effect=_fake_build), \
             unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok"):
            result = mc.control_set_zone_coupling(zone=0, from_zone=2, coeff=24.52, confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("timing_profiles", result)


class EndToEndRealBodyTest(_Base):
    def test_tool_posts_only_the_target_cell_in_bench_orientation(self):
        """Tool -> real build_post_body() -> posted form body. zone=2,
        from_zone=0 (bench z2[0]) must reach the wire as z2_coupling_c0 and
        NOTHING else coupling/model-shaped (those are omit-preserved by
        parse_zone_fields()); every remaining field must equal a plain echo
        of the GET snapshot. A transposed mapping would post z0_coupling_c2."""
        import urllib.parse
        sys.path.insert(0, os.path.dirname(__file__))
        import test_zones_http_client as tz
        cur = tz._sample_get_response()
        for i in range(3):
            for j in range(3):
                cur["zones"][i][f"coupling_c{j}"] = 0.0 if i == j else 10.0 * i + j + 1.5
            cur["zones"][i].update(model_k_dc=33.8493, model_tau_s=247.1, model_dead_time_s=26.0,
                                   coupling_diag_k_dc=12.3456)
        after = copy.deepcopy(cur)
        after["zones"][2]["coupling_c0"] = 8.08
        echo = dict(urllib.parse.parse_qsl(zones_http_client.build_post_body(cur, {"zones": []})))
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=[cur, after]),              unittest.mock.patch.object(zones_http_client, "post_zones", return_value="ok") as post_mock:
            result = mc.control_set_zone_coupling(zone=2, from_zone=0, coeff=8.08, confirm=True)
        self.assertIn("ok - zone 2", result)
        posted = dict(urllib.parse.parse_qsl(post_mock.call_args[0][1]))
        self.assertEqual(posted["z2_coupling_c0"], repr(8.08))
        stripped = {k for k in echo if mc._ZONE_OMIT_PRESERVED_KEY_RE.match(k)}
        # 3 cells + k/tau/deadtime/diag + six omit-preserved tuning floats, per zone
        self.assertEqual(len(stripped), 3 * (3 + 4 + 6))
        self.assertEqual(set(posted) - set(echo), set())
        self.assertEqual(set(echo) - set(posted), stripped - {"z2_coupling_c0"})
        for k, v in posted.items():
            if k != "z2_coupling_c0":
                self.assertEqual(v, echo[k], k)
        for required in ("z2_kp", "z2_ki", "z2_kd", "z0_kp", "z1_kp"):
            self.assertIn(required, posted)

    def test_strip_refuses_a_body_without_the_target_key(self):
        with self.assertRaises(zones_http_client.ZonesHttpError):
            mc._strip_omit_preserved_zone_fields("z0_kp=1.0&z0_coupling_c1=5.0", "z0_coupling_c2")


if __name__ == "__main__":
    unittest.main()
