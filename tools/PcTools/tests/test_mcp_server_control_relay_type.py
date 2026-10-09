#!/usr/bin/env python3
"""Unit tests for mcp_server_control.control_set_relay_type() -- the MCP tool
that writes ONE relay's device type (POST key relay<N>_type, GET top-level
relay_types[N-1]) over the GET-merge-POST /api/zones path. Modeled on
test_mcp_server_control_zone_coupling.py. The fake board below is stateful:
it serves GET from a dict and applies a posted relay<N>_type to it, so the
real build_post_body() and the tool's strip/append/read-back logic all run.
No real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_control_relay_type.py -q
"""
from __future__ import annotations

import copy
import os
import re
import sys
import unittest
import unittest.mock
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_control as mc  # noqa: E402
from kilnctrl import zones_http_client  # noqa: E402
import test_zones_http_client as tz  # noqa: E402


def _board_state(types=(0, 0, 0, 0)):
    cur = tz._sample_get_response()
    cur["relay_types"] = list(types)
    for i, z in enumerate(cur["zones"]):
        for j in range(3):
            z[f"coupling_c{j}"] = 0.0 if i == j else 10.0 * i + j + 1.5
        z.update(model_k_dc=33.8493, model_tau_s=247.1, model_dead_time_s=26.0, coupling_diag_k_dc=12.3456)
    return cur


class FakeBoard:
    """Serves GET from `state`; POST applies relay<N>_type like the firmware
    (omit-preserve, 400-style refusal text on a bad value). Hooks let a test
    simulate a POST that doesn't land or lands with collateral."""

    def __init__(self, state, post_result="ok", land=True, collateral=None, post_exc=None):
        self.state = state
        self.post_result = post_result
        self.land = land
        self.collateral = collateral
        self.post_exc = post_exc
        self.posted = []

    def get(self, host, *a, **k):
        return copy.deepcopy(self.state)

    def post(self, host, body, *a, **k):
        self.posted.append(body)
        if self.post_exc is not None:
            raise self.post_exc
        if self.post_result != "ok":
            return self.post_result
        fields = dict(urllib.parse.parse_qsl(body, keep_blank_values=True))
        for r in range(1, len(self.state["relay_types"]) + 1):
            key = f"relay{r}_type"
            if key in fields and self.land:
                self.state["relay_types"][r - 1] = int(fields[key])
        if self.collateral:
            self.collateral(self.state)
        return "ok"

    def patches(self):
        return [
            unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=self.get),
            unittest.mock.patch.object(zones_http_client, "post_zones", side_effect=self.post),
        ]


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

    def run_tool(self, board, **kwargs):
        ps = board.patches()
        for p in ps:
            p.start()
            self.addCleanup(p.stop)
        return mc.control_set_relay_type(**kwargs)


class HappyPathTest(_Base):
    def test_sets_by_name_and_confirms(self):
        board = FakeBoard(_board_state((0, 0, 0, 0)))
        result = self.run_tool(board, relay=3, device_type="Fan", confirm=True)
        self.assertIn("ok - relay3_type=4 (fan)", result)
        self.assertEqual(board.state["relay_types"], [0, 0, 4, 0])
        self.assertEqual(len(board.posted), 1)

    def test_sets_by_number_and_digit_string(self):
        for dt, want in ((1, 1), ("6", 6), (0, 0)):
            board = FakeBoard(_board_state((2, 2, 2, 2)))
            result = self.run_tool(board, relay=1, device_type=dt, confirm=True)
            self.assertIn("ok - relay1_type", result, (dt, result))
            self.assertEqual(board.state["relay_types"][0], want)

    def test_post_carries_exactly_one_relay_key_and_strips_omit_preserved(self):
        state = _board_state((1, 2, 3, 4))
        board = FakeBoard(state)
        echo = dict(urllib.parse.parse_qsl(zones_http_client.build_post_body(copy.deepcopy(state), {"zones": []})))
        result = self.run_tool(board, relay=2, device_type="valve", confirm=True)
        self.assertIn("ok - relay2_type=3 (valve)", result)
        posted = dict(urllib.parse.parse_qsl(board.posted[0]))
        self.assertEqual(posted["relay2_type"], "3")
        self.assertEqual([k for k in posted if re.match(r"^relay\d+_", k)], ["relay2_type"])
        stripped = {k for k in echo if mc._ZONE_OMIT_PRESERVED_KEY_RE.match(k)}
        # 3 cells + k/tau/deadtime/diag + six omit-preserved tuning floats, per zone
        self.assertEqual(len(stripped), 3 * (3 + 4 + 6))
        for tuning in ("fuzzy_strength", "easeoffmult", "approachratecap", "errorband", "rateband", "progressband"):
            self.assertNotIn(f"z0_{tuning}", posted, tuning)
        self.assertEqual(set(echo) - set(posted), stripped)
        self.assertEqual(set(posted) - set(echo), {"relay2_type"})
        for k, v in posted.items():
            if k != "relay2_type":
                self.assertEqual(v, echo[k], k)

    def test_dry_run_prints_current_name(self):
        board = FakeBoard(_board_state((0, 5, 0, 0)))
        result = self.run_tool(board, relay=2, device_type="damper")
        self.assertIn("DRY RUN", result)
        self.assertIn("relay2_type=1 (damper)", result)
        self.assertIn("current: 5 (light)", result)


class ConfirmGateTest(_Base):
    def test_confirm_must_be_exactly_true(self):
        for c in (False, 1, "yes", None):
            board = FakeBoard(_board_state())
            result = self.run_tool(board, relay=1, device_type="fan", confirm=c)
            self.assertIn("DRY RUN", result, c)
            self.assertEqual(board.posted, [])


class ArgRangeTest(_Base):
    def test_bad_relay_refused_without_post(self):
        for bad in (0, 5, -1, 99, True, "1", 1.0):
            board = FakeBoard(_board_state())
            result = self.run_tool(board, relay=bad, device_type="fan", confirm=True)
            self.assertTrue(result.startswith("refused"), (bad, result))
            self.assertEqual(board.posted, [], bad)

    def test_bad_device_type_refused_before_any_io(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            for bad in (-1, 7, 200, True, False, 2.0, "toaster", "", None, "7"):
                result = mc.control_set_relay_type(relay=1, device_type=bad, confirm=True)
                self.assertTrue(result.startswith("refused"), (bad, result))
        get_mock.assert_not_called()

    def test_boundary_values_accepted(self):
        for dt in (0, 6):
            board = FakeBoard(_board_state((3, 3, 3, 3)))
            self.assertIn("ok - relay4_type", self.run_tool(board, relay=4, device_type=dt, confirm=True))

    def test_board_without_relay_types_refused(self):
        state = _board_state()
        del state["relay_types"]
        board = FakeBoard(state)
        result = self.run_tool(board, relay=1, device_type="fan", confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertEqual(board.posted, [])


class RunningRefusalTest(_Base):
    def test_running_profile_refuses_before_get(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                         return_value=unittest.mock.Mock(state_name="running", profile_id=3, name="bisque")), \
             unittest.mock.patch.object(zones_http_client, "get_zones") as get_mock:
            result = mc.control_set_relay_type(relay=1, device_type="fan", confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("running", result)
        get_mock.assert_not_called()

    def test_running_autotune_refuses(self):
        with unittest.mock.patch.object(mc._srv._autotune, "get_status",
                                         return_value=unittest.mock.Mock(state=2, state_name="stepping", zone=1)):
            result = mc.control_set_relay_type(relay=1, device_type="fan", confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("autotune", result.lower())

    def test_system_mode_gate_409_is_surfaced(self):
        err = zones_http_client.ZonesHttpError(
            "POST /api/zones refused: HTTP 409", 409,
            "refused -- a firing or autotune run is active; zone configuration cannot be "
            "changed until it ends")
        board = FakeBoard(_board_state(), post_exc=err)
        result = self.run_tool(board, relay=1, device_type="fan", confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("system_mode_gate", result)
        self.assertIn("409", result)

    def test_post_not_ok_is_surfaced(self):
        board = FakeBoard(_board_state(), post_result="relay device type out of range")
        result = self.run_tool(board, relay=1, device_type="fan", confirm=True)
        self.assertTrue(result.startswith("refused"), result)
        self.assertIn("out of range", result)


class ReadBackMismatchTest(_Base):
    def test_value_did_not_land_fails_loud(self):
        """Negative-test target: with the read-back comparison removed this
        would report 'ok'."""
        board = FakeBoard(_board_state((0, 0, 0, 0)), land=False)
        result = self.run_tool(board, relay=2, device_type="fan", confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("read-back does not confirm", result)
        self.assertNotIn("ok - relay", result)

    def test_wrong_value_landed_fails_loud(self):
        def clobber(state):
            state["relay_types"][1] = 6
        board = FakeBoard(_board_state(), collateral=clobber)
        result = self.run_tool(board, relay=2, device_type="fan", confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("read-back does not confirm", result)
        self.assertIn("wanted 4", result)


class CollateralChangeTest(_Base):
    def test_other_relay_type_change_fails_loud(self):
        def drift(state):
            state["relay_types"][3] = 5
        board = FakeBoard(_board_state((0, 0, 0, 0)), collateral=drift)
        result = self.run_tool(board, relay=1, device_type="fan", confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("relay_types", result)
        self.assertNotIn("ok - relay", result)

    def test_zone_field_drift_fails_loud(self):
        def drift(state):
            state["zones"][0]["pid_kp"] = state["zones"][0]["pid_kp"] + 1.0
        board = FakeBoard(_board_state(), collateral=drift)
        result = self.run_tool(board, relay=1, device_type="fan", confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("pid_kp", result)

    def test_top_level_drift_fails_loud(self):
        def drift(state):
            state["timing_profiles"][0]["name"] = "Changed"
        board = FakeBoard(_board_state(), collateral=drift)
        result = self.run_tool(board, relay=1, device_type="fan", confirm=True)
        self.assertIn("FAILED", result)
        self.assertIn("timing_profiles", result)


class DescribeAndEnumTest(unittest.TestCase):
    def test_describe_relay_types_names_each_relay(self):
        text = mc._describe_relay_types({"relay_types": [0, 1, 4, 6]})
        for frag in ("relay1=0 (unset)", "relay2=1 (damper)", "relay3=4 (fan)", "relay4=6 (other)"):
            self.assertIn(frag, text)

    def test_describe_relay_types_missing_and_unknown(self):
        self.assertIn("not present", mc._describe_relay_types({}))
        self.assertIn("relay1=9 (?)", mc._describe_relay_types({"relay_types": [9]}))

    def test_enum_mirror_matches_firmware_header(self):
        header = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW", "App",
                              "drivers", "persist", "zones_config_accessors.h")
        text = open(header, encoding="utf-8").read()
        found = {int(v): n.lower() for n, v in re.findall(r"RELAY_DEVICE_TYPE_([A-Z]+)\s*=\s*(\d+)", text)}
        self.assertEqual(found, mc._RELAY_DEVICE_TYPE_NAMES)

    def test_post_key_matches_firmware(self):
        src = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW", "App",
                           "drivers", "http", "zones_http_post.c")
        text = open(src, encoding="utf-8").read()
        self.assertIn('"relay%u_type"', text)


if __name__ == "__main__":
    unittest.main()
