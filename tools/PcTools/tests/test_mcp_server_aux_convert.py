#!/usr/bin/env python3
"""Unit tests for mcp_server_aux.control_convert_onoff_zone_to_aux against a fake board:
zones_http_client.get_zones and aux_http_client's reads/POST are mocked; no socket.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_aux_convert.py -q
"""
from __future__ import annotations

import copy
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import aux_http_client as ahc  # noqa: E402
from kilnctrl import mcp_server_aux as ma  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import zones_http_client  # noqa: E402

ZONE = 1
RELAY = 3
DEST = 8 + RELAY - 1


def _zones(zone_type=1, relay_mask=0b0100, thermo=0b0010, failsafe=0):
    return {"zones": [
        {"zone_type": 0, "relay_mask": 0b0001, "thermo_mask": 0b0001, "failsafe_state": 0},
        {"zone_type": zone_type, "relay_mask": relay_mask, "thermo_mask": thermo, "failsafe_state": failsafe},
        {"zone_type": 1, "relay_mask": 0b0010, "thermo_mask": 0b0100, "failsafe_state": 0},
    ]}


def _zones_after():
    z = _zones()
    z["zones"][ZONE].update(zone_type=0, relay_mask=0)
    return z


def _entry(relay, **kw):
    e = {"relay": relay, "enabled": False, "conflicted": False, "tc_zone": -1,
         "hyst_c": 2.0, "min_on_s": 10, "min_off_s": 20}
    e.update(kw)
    return e


def _aux(by_relay=None, quarantined=False):
    by_relay = by_relay or {}
    return {"quarantined": quarantined, "enabled_mask": 0, "conflict_mask": 0, "zones_relay_mask": 0b0111,
            "relays": [_entry(r, **by_relay.get(r, {})) for r in (1, 2, 3, 4)]}


def _aux_after(tc=ZONE):
    return _aux({RELAY: {"enabled": True, "tc_zone": tc}})


def _rule(zone, **kw):
    r = {"zone": zone, "segment": 0, "temp_source": 0, "temp_cmp": 0}
    r.update(kw)
    return r


def _profiles():
    return {0: [_rule(ZONE), _rule(ZONE, segment=1, temp_source=1, temp_cmp=1)],
            1: [_rule(2)],
            2: [_rule(ZONE, segment=2)],
            3: []}


def _profiles_after(before=None):
    before = before or _profiles()
    return {pid: [dict(r, zone=DEST) if r["zone"] == ZONE else dict(r) for r in rules]
            for pid, rules in before.items()}


class ConvertTest(unittest.TestCase):
    def setUp(self):
        for p in (unittest.mock.patch.object(ma, "_running_reason", return_value=None),
                  unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")):
            p.start()
            self.addCleanup(p.stop)

    def _run(self, *, zones=None, aux=None, profiles=None, after_zones=None, after_aux=None,
             after_profiles=None, post=None, io_hits=None, **kw):
        zones = zones if zones is not None else _zones()
        aux = aux if aux is not None else _aux()
        profiles = profiles if profiles is not None else _profiles()
        kw.setdefault("zone", ZONE)
        kw.setdefault("confirm", True)
        zs = [zones, after_zones if after_zones is not None else _zones_after()]
        ax = [aux, after_aux if after_aux is not None else _aux_after()]
        pr = [profiles, after_profiles if after_profiles is not None else _profiles_after(profiles)]
        ack = {"ok": True} if post is None else post
        with unittest.mock.patch.object(zones_http_client, "get_zones", side_effect=zs), \
             unittest.mock.patch.object(ahc, "get_aux_outputs", side_effect=ax), \
             unittest.mock.patch.object(ahc, "get_stored_profile_rules", side_effect=pr), \
             unittest.mock.patch.object(ahc, "get_stored_relay_io_hits", return_value=io_hits or {}), \
             unittest.mock.patch.object(ahc, "post_move_zone_to_aux",
                                        side_effect=ack if isinstance(ack, Exception) else None,
                                        return_value=None if isinstance(ack, Exception) else ack) as p:
            r = ma.control_convert_onoff_zone_to_aux(**kw)
        return r, p

    def _refused(self, **kw):
        # Prechecks refuse before any POST; only the first read set is consumed.
        r, p = self._run(**kw)
        self.assertTrue(r.startswith("refused"), r)
        p.assert_not_called()
        return r

    def test_happy_path(self):
        r, p = self._run()
        self.assertTrue(r.startswith("ok"), r)
        p.assert_called_once_with("10.0.0.5", ZONE)
        self.assertIn("3 rule(s) in 2 of 4", r)

    def test_confirm_gate_exactly_true(self):
        for bad in (False, 1, "yes", None):
            r, p = self._run(confirm=bad)
            self.assertIn("DRY RUN", r, (bad, r))
            p.assert_not_called()

    def test_arg_validation_before_any_io(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones") as g:
            for z in (-1, 3, True, "1", None, 1.0):
                r = ma.control_convert_onoff_zone_to_aux(zone=z, confirm=True)
                self.assertTrue(r.startswith("refused"), (z, r))
        g.assert_not_called()

    def test_refuses_mid_run_precheck(self):
        with unittest.mock.patch.object(ma, "_running_reason", return_value="a profile is currently running"), \
             unittest.mock.patch.object(zones_http_client, "get_zones") as g, \
             unittest.mock.patch.object(ahc, "post_move_zone_to_aux") as p:
            r = ma.control_convert_onoff_zone_to_aux(zone=ZONE, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        g.assert_not_called()
        p.assert_not_called()

    def test_refuses_not_on_off(self):
        self.assertIn("not an ON_OFF", self._refused(zones=_zones(zone_type=0)))

    def test_refuses_failsafe_on(self):
        self.assertIn("failsafe", self._refused(zones=_zones(failsafe=1)))

    def test_refuses_multi_or_no_relay(self):
        self._refused(zones=_zones(relay_mask=0b0110))
        self._refused(zones=_zones(relay_mask=0))

    def test_refuses_relay_beyond_aux_range(self):
        self._refused(zones=_zones(relay_mask=0b10000))

    def test_refuses_quarantined_store(self):
        self._refused(aux=_aux(quarantined=True))

    def test_refuses_relay_already_bound(self):
        self._refused(aux=_aux({RELAY: {"enabled": True}}))
        self._refused(aux=_aux({RELAY: {"conflicted": True}}))

    def test_refuses_profile_rule_already_at_destination(self):
        prof = _profiles()
        prof[3] = [_rule(DEST)]
        self.assertIn("already has a rule", self._refused(profiles=prof))

    def test_refuses_unrepresentable_rules(self):
        prof = _profiles()
        prof[0][0]["temp_source"] = 2
        self.assertIn("temp_source", self._refused(profiles=prof))
        prof = _profiles()
        self.assertIn("thermocouple", self._refused(profiles=prof, zones=_zones(thermo=0)))

    def test_no_thermocouple_zone_without_temp_rules_is_allowed(self):
        prof = {0: [_rule(ZONE)]}
        r, p = self._run(zones=_zones(thermo=0), profiles=prof, after_aux=_aux_after(tc=-1))
        self.assertTrue(r.startswith("ok"), r)
        p.assert_called_once()

    def test_dry_run_reports_plan_and_never_posts(self):
        r, p = self._run(confirm=False)
        self.assertIn("DRY RUN", r)
        self.assertIn("3 rule(s)", r)
        p.assert_not_called()

    def test_dry_run_lists_relay_io_segments_that_target_the_relay(self):
        r, p = self._run(confirm=False, io_hits={2: [1, 3], 5: [2]})
        self.assertIn("DRY RUN", r)
        self.assertIn("RELAY_IO", r)
        self.assertIn("profile 2 segment(s) 1, 3", r)
        self.assertIn("profile 5 segment(s) 2", r)
        p.assert_not_called()

    def test_no_relay_io_note_when_none(self):
        r, _ = self._run(confirm=False)
        self.assertNotIn("RELAY_IO", r)

    def test_resume_validation_and_gate(self):
        with unittest.mock.patch.object(ahc, "post_move_zone_to_aux") as p, \
             unittest.mock.patch.object(zones_http_client, "get_zones") as g:
            for bad in (-1, 5, True, "3", 1.0):
                r = ma.control_convert_onoff_zone_to_aux(zone=ZONE, confirm=True, resume_relay=bad)
                self.assertTrue(r.startswith("refused"), (bad, r))
            r = ma.control_convert_onoff_zone_to_aux(zone=ZONE, confirm=False, resume_relay=3)
            self.assertIn("DRY RUN", r)
            p.assert_not_called()
            g.assert_not_called()

    def _resume(self, zones_after=None, aux_after=None, readiness=None, zones_before=None, aux_before=None):
        zs = [zones_before or _zones_after(), zones_after or _zones_after()]
        ax = [aux_before or _aux_after(), aux_after or _aux_after()]
        with unittest.mock.patch.object(ahc, "post_move_zone_to_aux", return_value={"ok": True}) as p,              unittest.mock.patch.object(zones_http_client, "get_zones",
                                        side_effect=zs),              unittest.mock.patch.object(ahc, "get_aux_outputs", side_effect=ax),              unittest.mock.patch.object(ahc, "get_stored_profile_rules", return_value={0: []}),              unittest.mock.patch.object(ma.readiness_http_client, "get_readiness",
                                        return_value=readiness or {"items": []}):
            r = ma.control_convert_onoff_zone_to_aux(zone=ZONE, confirm=True, resume_relay=3)
        return r, p

    def test_resume_posts_resume_fields_only_and_reads_back(self):
        r, p = self._resume()
        self.assertTrue(r.startswith("ok - resumed"), r)
        p.assert_called_once_with("10.0.0.5", ZONE, resume_relay=3)

    def test_resume_readback_zone_unchanged_fails_loud(self):
        r, _ = self._resume(zones_after=_zones())
        self.assertTrue(r.startswith("FAILED"), r)

    def test_resume_readback_aux_not_enabled_fails_loud(self):
        r, _ = self._resume(aux_after=_aux())
        self.assertTrue(r.startswith("FAILED"), r)

    def test_resume_wrong_tc_zone_fails_loud(self):
        r, _ = self._resume(aux_after=_aux_after(tc=2))
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("tc_zone", r)

    def test_resume_marker_still_present_fails_loud(self):
        r, _ = self._resume(readiness={"items": [{"key": "zone_aux_conversion", "detail": "stage 2"}]})
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("zone_aux_conversion", r)

    def test_resume_other_zone_changed_fails_loud(self):
        za = _zones_after()
        za["zones"][2]["relay_mask"] = 0
        r, _ = self._resume(zones_after=za)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("zone 2", r)

    def test_resume_other_aux_changed_fails_loud(self):
        aa = _aux({RELAY: {"enabled": True, "tc_zone": ZONE}, 1: {"enabled": True}})
        r, _ = self._resume(aux_after=aa)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("aux relay 1", r)

    def test_resume_409_is_a_refusal(self):
        exc = ahc.AuxHttpError("x", 409, "no interrupted conversion is recorded -- nothing to resume")
        with unittest.mock.patch.object(ahc, "post_move_zone_to_aux", side_effect=exc), \
             unittest.mock.patch.object(zones_http_client, "get_zones", return_value=_zones()), \
             unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_aux()):
            r = ma.control_convert_onoff_zone_to_aux(zone=ZONE, confirm=True, resume_relay=3)
        self.assertTrue(r.startswith("refused"), r)

    def test_409_gate_is_a_refusal(self):
        exc = ahc.AuxHttpError("x", 409, "refused -- a firing or autotune run is active")
        r, _ = self._run(post=exc)
        self.assertTrue(r.startswith("refused"), r)

    def test_500_is_an_error(self):
        r, _ = self._run(post=ahc.AuxHttpError("x", 500, "ROLLBACK INCOMPLETE"))
        self.assertTrue(r.startswith("error"), r)

    def test_readback_zone_unchanged_fails_loud(self):
        r, _ = self._run(after_zones=_zones())
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_aux_not_enabled_fails_loud(self):
        r, _ = self._run(after_aux=_aux())
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_wrong_tc_zone_fails_loud(self):
        r, _ = self._run(after_aux=_aux_after(tc=0))
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_profiles_not_rewritten_fails_loud(self):
        r, _ = self._run(after_profiles=_profiles())
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_partial_rewrite_fails_loud(self):
        after = _profiles_after()
        after[2] = [_rule(ZONE, segment=2)]  # one profile left behind
        r, _ = self._run(after_profiles=after)
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_collateral_rule_change_fails_loud(self):
        after = _profiles_after()
        after[1] = [_rule(2, segment=9)]
        r, _ = self._run(after_profiles=after)
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_collateral_other_zone_fails_loud(self):
        z = _zones_after()
        z["zones"][2]["relay_mask"] = 0
        r, _ = self._run(after_zones=z)
        self.assertTrue(r.startswith("FAILED"), r)

    def test_readback_collateral_other_aux_relay_fails_loud(self):
        a = _aux_after()
        a["relays"][0]["enabled"] = True
        r, _ = self._run(after_aux=a)
        self.assertTrue(r.startswith("FAILED"), r)

    def test_confirming_reread_failure_reports_unknown_state(self):
        with unittest.mock.patch.object(zones_http_client, "get_zones",
                                        side_effect=[_zones(), zones_http_client.ZonesHttpError("down")]), \
             unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_aux()), \
             unittest.mock.patch.object(ahc, "get_stored_profile_rules", return_value=_profiles()), \
             unittest.mock.patch.object(ahc, "get_stored_relay_io_hits", return_value={}), \
             unittest.mock.patch.object(ahc, "post_move_zone_to_aux", return_value={"ok": True}):
            r = ma.control_convert_onoff_zone_to_aux(zone=ZONE, confirm=True)
        self.assertTrue(r.startswith("error"), r)
        self.assertIn("UNKNOWN", r)

    def test_inputs_not_mutated(self):
        zones, prof = _zones(), _profiles()
        z0, p0 = copy.deepcopy(zones), copy.deepcopy(prof)
        self._run(zones=zones, profiles=prof)
        self.assertEqual(zones, z0)
        self.assertEqual(prof, p0)


class PostMoveClientTest(unittest.TestCase):
    def test_posts_exactly_the_two_fields(self):
        sent = {}

        class _Resp:
            status = 200

            def read(self):
                return b'{"ok":true,"zone":1,"relay":3,"profiles_scanned":4,"profiles_affected":2,"rules_retargeted":3}'

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        def fake_urlopen(req, timeout=None):
            sent["url"] = req.full_url
            sent["data"] = req.data
            return _Resp()

        with unittest.mock.patch.object(ahc.http_auth, "urlopen", side_effect=fake_urlopen):
            out = ahc.post_move_zone_to_aux("10.0.0.5", 1)
        self.assertTrue(sent["url"].endswith("/api/zones"))
        self.assertEqual(sent["data"], b"move_zone_to_aux=1&confirm=1")
        self.assertTrue(out["ok"])

    def test_resume_fields(self):
        sent = {}

        class _Resp:
            status = 200

            def read(self):
                return b'{"ok":true}'

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        def fake_urlopen(req, timeout=None):
            sent["data"] = req.data
            return _Resp()

        with unittest.mock.patch.object(ahc.http_auth, "urlopen", side_effect=fake_urlopen):
            ahc.post_move_zone_to_aux("10.0.0.5", 1, resume_relay=3)
        self.assertEqual(sent["data"], b"move_zone_to_aux=1&confirm=1&resume=1&relay=3")


class RelayIoHitsTest(unittest.TestCase):
    def test_lists_only_matching_relay_io_segments_of_stored_profiles(self):
        listing = [{"id": 0, "builtin": True}, {"id": 1}, {"id": 2}]
        detail = {
            1: {"segments": [{"seg_kind": 0}, {"seg_kind": 1, "io_target": 3}, {"seg_kind": 1, "io_target": 2}]},
            2: {"segments": [{"seg_kind": 1, "io_target": 1}]},
        }

        def fake_get(host, path, timeout):
            if path == "/api/profiles":
                return listing
            return detail[int(path.rsplit("=", 1)[1])]

        with unittest.mock.patch.object(ahc, "_get_json", side_effect=fake_get):
            self.assertEqual(ahc.get_stored_relay_io_hits("h", 3), {1: [2]})
            self.assertEqual(ahc.get_stored_relay_io_hits("h", 4), {})


if __name__ == "__main__":
    unittest.main()
