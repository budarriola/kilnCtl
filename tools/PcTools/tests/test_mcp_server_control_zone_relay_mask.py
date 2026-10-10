#!/usr/bin/env python3
"""Unit tests for mcp_server_control.control_set_zone_relay_mask(), modeled on
test_mcp_server_control_zone_type.py. Mocked zones_http_client; no board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_control_zone_relay_mask.py -q
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


def _zone(index, **o):
    z = {"index": index, "max_temp_c": 80.0, "pid_kp": 1.0, "control_mode": 1,
         "relay_mask": 1 << index, "zone_type": 0}
    z.update(o)
    return z


def _snap(zones):
    return {"thermo_count": len(zones), "relay_count": 4, "zones": zones}


class _Base(unittest.TestCase):
    def setUp(self):
        for p in (
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
            unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                       return_value=unittest.mock.Mock(state_name="idle", profile_id=0, name="")),
            unittest.mock.patch.object(mc._srv._autotune, "get_status",
                                       return_value=unittest.mock.Mock(state=0, state_name="idle", zone=0)),
        ):
            p.start()
            self.addCleanup(p.stop)

    def run_tool(self, before, after=None, post="ok", build="body", **kw):
        gets = [before] if after is None else [before, after]
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=gets), \
             unittest.mock.patch.object(zones_http_client, "build_post_body", return_value=build) as b, \
             unittest.mock.patch.object(zones_http_client, "post_zones",
                                        **({"side_effect": post} if isinstance(post, Exception) else {"return_value": post})) as p:
            r = mc.control_set_zone_relay_mask(**kw)
        return r, b, p


class GateTest(_Base):
    def test_bad_mask_refused_before_io(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones") as g:
            for bad in (-1, 0x10000, True, "3", 1.0):
                self.assertTrue(mc.control_set_zone_relay_mask(zone=0, relay_mask=bad, confirm=True).startswith("refused"))
        g.assert_not_called()

    def test_dry_run_and_truthy_confirm_never_post(self):
        for c in (False, 1):
            r, _b, p = self.run_tool(_snap([_zone(0)]), zone=0, relay_mask=9, confirm=c)
            self.assertIn("DRY RUN", r)
            p.assert_not_called()

    def test_running_refuses(self):
        with unittest.mock.patch.object(mc._srv._profiles, "get_exec_status",
                                        return_value=unittest.mock.Mock(state_name="running", profile_id=3, name="b")), \
             unittest.mock.patch.object(zones_http_client, "get_zones") as g:
            r = mc.control_set_zone_relay_mask(zone=0, relay_mask=1, confirm=True)
        self.assertTrue(r.startswith("refused"))
        g.assert_not_called()

    def test_unknown_zone(self):
        r, _b, _p = self.run_tool(_snap([_zone(0)]), zone=5, relay_mask=1, confirm=True)
        self.assertIn("out of range", r)


class WriteTest(_Base):
    def test_applies_and_confirms(self):
        before = _snap([_zone(0), _zone(1)])
        after = copy.deepcopy(before)
        after["zones"][0]["relay_mask"] = 5
        r, b, p = self.run_tool(before, after, zone=0, relay_mask=5, confirm=True)
        self.assertIn("ok - zone 0: relay_mask=5", r)
        b.assert_called_once_with(before, {"zones": [{"index": 0, "relay_mask": 5}]})
        p.assert_called_once()

    def test_firmware_400_is_reported_as_refusal(self):
        err = zones_http_client.ZonesHttpError("x", 400, "zone relay_mask references an aux relay")
        r, _b, _p = self.run_tool(_snap([_zone(0)]), post=err, zone=0, relay_mask=9, confirm=True)
        self.assertIn("refused by firmware (HTTP 400)", r)

    def test_mode_gate_409(self):
        err = zones_http_client.ZonesHttpError(
            "x", 409, "refused -- a firing or autotune run is active; zone configuration cannot be changed until it ends")
        r, _b, _p = self.run_tool(_snap([_zone(0)]), post=err, zone=0, relay_mask=9, confirm=True)
        self.assertIn("system_mode_gate", r)

    def test_not_landed_fails_loud(self):
        before = _snap([_zone(0)])
        r, _b, _p = self.run_tool(before, copy.deepcopy(before), zone=0, relay_mask=5, confirm=True)
        self.assertIn("FAILED", r)
        self.assertNotIn("ok - zone", r)

    def test_collateral_fails_loud(self):
        before = _snap([_zone(0), _zone(1)])
        after = copy.deepcopy(before)
        after["zones"][0]["relay_mask"] = 5
        after["zones"][1]["relay_mask"] = 7
        r, _b, _p = self.run_tool(before, after, zone=0, relay_mask=5, confirm=True)
        self.assertIn("FAILED", r)
        self.assertIn("zone 1", r)


if __name__ == "__main__":
    unittest.main()
