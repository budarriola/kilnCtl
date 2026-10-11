#!/usr/bin/env python3
"""Unit tests for mcp_server_aux.profile_save_bench_aux_rule against a stateful
fake board (aux_http_client._get_json/get_aux_outputs and
profile_edit_http_client.post_profile are replaced; no socket, no live board).

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_aux_rule_profile.py -q
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
from kilnctrl import profile_edit_http_client as pehc  # noqa: E402

NAME = ma.BENCH_AUX_PROFILE_NAME


def _aux(**relay4):
    e = {"relay": 4, "enabled": True, "conflicted": False, "tc_zone": 0, "hyst_c": 2.0,
         "min_on_s": 10, "min_off_s": 20}
    e.update(relay4)
    return {"quarantined": False, "relays": [
        {"relay": r, "enabled": False, "conflicted": False, "tc_zone": -1, "hyst_c": 2.0,
         "min_on_s": 10, "min_off_s": 20} for r in (1, 2, 3)] + [e]}


class FakeBoard:
    """Stores user profiles by id; mimics POST id=-1 -> first free slot."""

    def __init__(self, profiles=None, lie=None, force_slot=None):
        self.profiles = copy.deepcopy(profiles or {})
        self.lie = lie or (lambda detail: detail)  # mutate the stored detail to simulate a bad write
        self.force_slot = force_slot
        self.posts = []

    def listing(self):
        return [{"id": i, "builtin": False, "name": p["name"], "zone_mask": p["zone_mask"],
                 "segment_count": len(p["segments"])} for i, p in sorted(self.profiles.items())] + \
               [{"id": 200, "builtin": True, "name": "Cone 6"}]

    def get_json(self, host, path, timeout):
        if path == "/api/profiles":
            return self.listing()
        pid = int(path.split("id=")[1])
        return copy.deepcopy(self.profiles[pid])

    def post_profile(self, host, profile_id, name, zone_mask, segments, on_off_rules=None, timeout=8.0):
        self.posts.append((profile_id, name, zone_mask, segments, on_off_rules))
        pid = profile_id
        if pid < 0:
            pid = next(i for i in range(100) if i not in self.profiles)
        if self.force_slot is not None:
            pid = self.force_slot
        r = on_off_rules[0]
        detail = {"id": pid, "name": name, "zone_mask": zone_mask, "segment_count": 1,
                  "segments": [{"target_c": segments[0].target_c, "ramp_c_per_hr": segments[0].ramp_c_per_hr,
                                "dwell_min": segments[0].dwell_min}],
                  "on_off_rules": [{"zone": r.zone_index, "segment": r.segment_index, "enable": 1,
                                    "phase_mask": 0, "direction_mask": 0, "temp_source": r.temp_source,
                                    "temp_cmp": r.temp_cmp, "temp_c": r.temp_threshold_c, "invert": 0}]}
        self.profiles[pid] = self.lie(detail)
        return {"ok": True, "id": pid}


def _user_profile(name):
    return {"name": name, "zone_mask": 1, "segments": [{"target_c": 50.0}], "on_off_rules": []}


class SaveBenchAuxRuleTest(unittest.TestCase):
    def setUp(self):
        for p in (unittest.mock.patch.object(ma, "_running_reason", return_value=None),
                  unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")):
            p.start()
            self.addCleanup(p.stop)

    def _run(self, board, aux=None, **kw):
        kw.setdefault("target_c", 30.0)
        kw.setdefault("threshold_c", 28.0)
        kw.setdefault("confirm", True)
        with unittest.mock.patch.object(ahc, "_get_json", side_effect=board.get_json), \
             unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=aux or _aux()), \
             unittest.mock.patch.object(pehc, "post_profile", side_effect=board.post_profile):
            return ma.profile_save_bench_aux_rule(**kw)

    def test_happy_path_creates_new_slot_and_leaves_user_profiles(self):
        board = FakeBoard({0: _user_profile("MY_BISQUE")})
        r = self._run(board)
        self.assertTrue(r.startswith("ok"), r)
        self.assertEqual(len(board.posts), 1)
        pid, name, zmask, segs, rules = board.posts[0]
        self.assertEqual((pid, name, zmask), (-1, NAME, 1))
        self.assertEqual(rules[0].zone_index, 11)  # aux relay 4 -> wire byte 11
        self.assertEqual(rules[0].temp_cmp, pehc.ON_OFF_TEMP_CMP_BELOW)
        self.assertEqual(rules[0].temp_source, 1)
        self.assertEqual(board.profiles[0]["name"], "MY_BISQUE")

    def test_other_profile_segment_content_change_fails(self):
        # name, zone_mask and segment_count unchanged, but a segment value moved:
        # the old name/mask/count comparison missed this (review 2026-10-10 MED).
        board = FakeBoard({0: _user_profile("MY_BISQUE")})
        orig = board.post_profile

        def post(*a, **k):
            r = orig(*a, **k)
            board.profiles[0]["segments"][0]["target_c"] = 999.0
            return r
        board.post_profile = post
        r = self._run(board)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("content changed", r)

    def test_reuses_only_its_own_named_slot(self):
        board = FakeBoard({0: _user_profile("MY_BISQUE"), 1: _user_profile(NAME)})
        r = self._run(board, temp_cmp="above")
        self.assertTrue(r.startswith("ok"), r)
        self.assertEqual(board.posts[0][0], 1)
        self.assertEqual(board.posts[0][4][0].temp_cmp, pehc.ON_OFF_TEMP_CMP_ABOVE)
        self.assertEqual(board.profiles[0]["name"], "MY_BISQUE")

    def test_confirm_gate_exactly_true(self):
        for bad in (False, 1, "yes", None):
            board = FakeBoard()
            r = self._run(board, confirm=bad)
            self.assertIn("DRY RUN", r, (bad, r))
            self.assertEqual(board.posts, [])

    def test_arg_validation_before_any_io(self):
        base = {"target_c": 30.0, "threshold_c": 28.0}
        bad_sets = ({"relay": 0}, {"relay": 5}, {"relay": True}, {"zone": 3}, {"temp_cmp": "equal"},
                    {"target_c": 150.0}, {"target_c": float("nan")}, {"threshold_c": -1.0},
                    {"threshold_c": True}, {"ramp_c_per_hr": 0}, {"dwell_min": -1})
        with unittest.mock.patch.object(ahc, "get_aux_outputs") as get:
            for extra in bad_sets:
                kw = dict(base, **extra)
                r = ma.profile_save_bench_aux_rule(confirm=True, **kw)
                self.assertTrue(r.startswith("refused"), (extra, r))
        get.assert_not_called()

    def test_refuses_mid_run(self):
        board = FakeBoard()
        with unittest.mock.patch.object(ma, "_running_reason", return_value="a profile is running"):
            r = self._run(board)
        self.assertTrue(r.startswith("refused"), r)
        self.assertEqual(board.posts, [])

    def test_refuses_aux_not_ready(self):
        for aux in (_aux(enabled=False), _aux(conflicted=True), _aux(tc_zone=-1)):
            board = FakeBoard()
            r = self._run(board, aux=aux)
            self.assertTrue(r.startswith("refused"), r)
            self.assertEqual(board.posts, [])

    def test_fails_loud_when_readback_rule_wrong(self):
        def lie(d):
            d["on_off_rules"][0]["temp_c"] = 99.0
            return d
        r = self._run(FakeBoard(lie=lie))
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("temp_c", r)

    def test_fails_loud_when_rule_target_wrong(self):
        def lie(d):
            d["on_off_rules"][0]["zone"] = 2
            return d
        self.assertTrue(self._run(FakeBoard(lie=lie)).startswith("FAILED"))

    def test_fails_loud_when_rule_missing(self):
        def lie(d):
            d["on_off_rules"] = []
            return d
        r = self._run(FakeBoard(lie=lie))
        self.assertTrue(r.startswith("FAILED"), r)

    def test_fails_loud_when_it_landed_in_a_user_slot(self):
        board = FakeBoard({0: _user_profile("MY_BISQUE")}, force_slot=0)
        r = self._run(board)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("MY_BISQUE", r)

    def test_post_refusal_reported(self):
        board = FakeBoard()
        with unittest.mock.patch.object(ahc, "_get_json", side_effect=board.get_json), \
             unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_aux()), \
             unittest.mock.patch.object(pehc, "post_profile",
                                        side_effect=pehc.ProfileEditHttpError("HTTP 400: rule 0: bad", 400, {})):
            r = ma.profile_save_bench_aux_rule(target_c=30.0, threshold_c=28.0, confirm=True)
        self.assertTrue(r.startswith("refused"), r)


class StoredFieldsTest(unittest.TestCase):
    """mcpfx2 L5: derived feasibility fields never count as another profile changing."""

    BASE = {"name": "p", "zone_mask": 1, "segments": [{"target_c": 100.0, "feasibility": "ok"}],
            "on_off_rules": [], "feasibility": "ok", "exceeds_ceiling": False, "ceiling_note": ""}

    def test_derived_fields_ignored(self):
        other = copy.deepcopy(self.BASE)
        other["feasibility"] = "infeasible"
        other["exceeds_ceiling"] = True
        other["segments"][0]["feasibility"] = "bad"
        self.assertEqual(ma._stored_profile_fields(self.BASE), ma._stored_profile_fields(other))

    def test_stored_fields_still_compared(self):
        other = copy.deepcopy(self.BASE)
        other["segments"][0]["target_c"] = 200.0
        self.assertNotEqual(ma._stored_profile_fields(self.BASE), ma._stored_profile_fields(other))


if __name__ == "__main__":
    unittest.main()
