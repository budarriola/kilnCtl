#!/usr/bin/env python3
"""Unit tests for kilnctrl.zones_http_client -- GET/merge/POST construction
and read-back verification, all against MOCKED urllib responses. No real
socket and no live board.

The centerpiece is ZoneClearingTest below: it proves the specific hazard
this module exists to prevent -- a partial preset POSTed without merging
against a live GET would silently zero every field it doesn't mention,
including max_temp_c (== "no ceiling" on a bench with elements physically
connected). See zones_http_client.py's module docstring.

Run with: python -m pytest tools/PcTools/tests/test_zones_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import zones_http_client as zh  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _sample_zone(index: int, **overrides) -> dict:
    z = {
        "index": index, "name": f"zone{index}", "relay_mask": 1 << index, "thermo_mask": 1 << index,
        "cal_offset_c": 0.0, "pid_kp": 2.0, "pid_ki": 0.1, "pid_kd": 1.0,
        "max_ramp_c_per_hr": 900.0, "sanity_rate_c_per_min": 0.0, "control_mode": 2,
        "max_temp_c": 80.0, "min_temp_c": 0.0, "heater_window_ms": 0.0, "heater_min_on_ms": 0.0,
        "heater_min_off_ms": 0.0, "guard_wrong_dir_window_s": 0.0, "guard_wrong_dir_rate_c_per_min": 0.0,
        "guard_off_settle_s": 0.0, "guard_runaway_rate_c_per_min": 0.0, "guard_runaway_margin_c": 0.0,
        "guard_drift_period_s": 0.0, "guard_sensor_fault_debounce_ticks": 0.0, "guard_frozen_window_s": 0.0,
        "cross_zone_max_delta_c": 0.0, "model_k_dc": 0.0, "model_tau_s": 0.0, "model_dead_time_s": 0.0,
        "tc_type": 3, "ct_mask": 0, "timing_profile": 0,
        "normal_current_measured": False, "normal_current_a": 0.0,
        "fuzzy_strength_pct": 0.0,
        # Coupling row: MAX31856_CHANNEL_COUNT (3, uart_task_ids.h's
        # THERMO_CHANNEL_COUNT) cells, diagonal (j == index) always 0 --
        # zones_http.c always emits the full row for every zone regardless
        # of thermo_count.
        **{f"coupling_c{j}": 0.0 for j in range(3)},
        "settings_source": 0xFF,
    }
    z.update(overrides)
    return z


def _sample_get_response(n_zones: int = 3) -> dict:
    return {
        "thermo_count": n_zones, "relay_count": 4, "max_simultaneous_relays": 0,
        "continue_on_zone_trip": False, "safety_tc_type": 3, "pc_link_abort_silence_ms": 0.0,
        "relay_zone_owned_mask": 0b0111,
        "safety_wiring": {"link_up": True, "tc_temp_valid": False, "tc_temp_c": 0.0,
                           "tc_fault": 0, "relay_energized": False},
        "ct_warn_mask": 0,
        "relay_names": ["", "", "", ""],
        "timing_profiles": [
            {"index": 0, "name": "Default", "guard_progress_duty_min": 0.0, "guard_progress_window_s": 0.0,
             "guard_drift_hysteresis_c": 0.0, "guard_frozen_eps_c": 0.0, "guard_cross_zone_period_s": 0.0,
             "bangbang_hysteresis_c": 0.0, "cooling_limited_margin_c": 0.0, "cooling_limited_hold_s": 0.0,
             "ramp_lock_band_c": 0.0},
        ],
        "zones": [_sample_zone(i) for i in range(n_zones)],
    }


def _mock_get(body: dict):
    return unittest.mock.patch.object(
        zh.urllib.request, "urlopen",
        return_value=_fake_response(json.dumps(body).encode()))


class GetZonesTest(unittest.TestCase):
    def test_parses_json(self):
        payload = _sample_get_response()
        with _mock_get(payload):
            result = zh.get_zones("kiln.local")
        self.assertEqual(result["thermo_count"], 3)
        self.assertEqual(len(result["zones"]), 3)

    def test_rejects_non_json(self):
        with unittest.mock.patch.object(zh.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(zh.ZonesHttpError):
                zh.get_zones("kiln.local")

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(zh.ZonesHttpError):
                zh.get_zones("192.0.2.1")


def _decode_body(body: str) -> dict:
    return dict(urllib.parse.parse_qsl(body, keep_blank_values=True))


class BuildPostBodyTest(unittest.TestCase):
    def test_echoes_every_current_field_when_preset_overrides_nothing(self):
        current = _sample_get_response()
        preset = {"name": "empty", "zones": []}
        body = zh.build_post_body(current, preset)
        form = _decode_body(body)
        self.assertEqual(form["thermo_count"], "3")
        self.assertEqual(form["relay_count"], "4")
        self.assertEqual(form["z0_maxtemp"], repr(80.0))
        self.assertEqual(form["z0_relay_mask"], "1")
        self.assertEqual(form["tp0_name"], "Default")

    def test_preset_override_lands_on_the_right_zone_only(self):
        current = _sample_get_response()
        preset = {
            "name": "p", "zones": [{"index": 1, "relay_mask": 1, "control_mode": 0,
                                     "cal_offset_c": 0.0, "pid_kp": 0.0, "pid_ki": 0.0, "pid_kd": 0.0,
                                     "max_ramp_c_per_hr": 0.0, "max_temp_c": 42.5, "min_temp_c": 0.0}],
        }
        form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["z1_maxtemp"], repr(42.5))
        # zone 0/2 untouched -- still the live 80.0 from GET, not zeroed or
        # clobbered by zone 1's override.
        self.assertEqual(form["z0_maxtemp"], repr(80.0))
        self.assertEqual(form["z2_maxtemp"], repr(80.0))

    def test_heater_min_on_ms_is_echoed_and_preset_overridable(self):
        """heater_min_on_ms became a real per-zone setting on 2026-08-28 (a
        10 s hardware-protection floor, HEATER_MIN_ON_MS_FLOOR), so a preset
        has to be able to pin it -- and a preset that does NOT mention it must
        still echo whatever the board reported rather than zeroing it, since
        POST /api/zones is a whole-page submit."""
        current = _sample_get_response()
        current["zones"][0]["heater_min_on_ms"] = 15000.0
        current["zones"][2]["heater_min_on_ms"] = 15000.0

        # Not mentioned by the preset -> echoed, not zeroed.
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["z0_minon"], repr(15000.0))

        # Mentioned -> overridden, and only on the named zone.
        preset = {"name": "p", "zones": [{"index": 0, "heater_min_on_ms": 12000.0}]}
        form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["z0_minon"], repr(12000.0))
        self.assertEqual(form["z2_minon"], repr(15000.0))

    def test_thermo_count_relay_count_overridable_by_preset(self):
        current = _sample_get_response()
        preset = {"name": "p", "thermo_count": 2, "relay_count": 2, "zones": []}
        form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["thermo_count"], "2")
        self.assertEqual(form["relay_count"], "2")

    # ---- THE NEGATIVE TEST: prove the whole-page-submit protection works ----

    def test_ZEROING_a_partial_preset_would_cause_is_prevented_by_the_merge(self):
        """THE central negative test this task exists for. First show what
        a NAIVE client (posting only the preset's own fields, ignoring GET
        entirely) would do: max_temp_c disappears from the body -- which,
        per zones_post_handler()'s whole-page-submit contract, means the
        board would set it to 0.0 ("no ceiling") on every zone the naive
        POST didn't mention it for. THEN prove build_post_body() does not
        have this property: max_temp_c is present and correct for every
        zone, not just the one the preset named."""
        current = _sample_get_response(n_zones=3)
        preset = {
            "name": "p",
            "zones": [{"index": 0, "relay_mask": 1, "control_mode": 0, "cal_offset_c": 0.0,
                       "pid_kp": 1.0, "pid_ki": 0.0, "pid_kd": 0.0, "max_ramp_c_per_hr": 0.0,
                       "max_temp_c": 80.0, "min_temp_c": 0.0}],
        }

        # RED: what a naive urlencode(preset-only-fields) body would look
        # like -- no z1_maxtemp/z2_maxtemp key at all.
        naive_body = urllib.parse.urlencode({"z0_maxtemp": "80.0"})
        naive_form = _decode_body(naive_body)
        self.assertNotIn("z1_maxtemp", naive_form,
                          "sanity check on the naive body itself: it must NOT carry zone 1's "
                          "max_temp_c, which is exactly the hazard this test proves the real "
                          "client avoids")
        self.assertNotIn("z2_maxtemp", naive_form)

        # GREEN: the real client's body carries max_temp_c for every zone,
        # sourced from the live GET, not zeroed.
        real_form = _decode_body(zh.build_post_body(current, preset))
        self.assertIn("z1_maxtemp", real_form)
        self.assertIn("z2_maxtemp", real_form)
        self.assertEqual(real_form["z1_maxtemp"], repr(80.0))
        self.assertEqual(real_form["z2_maxtemp"], repr(80.0))
        self.assertNotEqual(real_form["z1_maxtemp"], "0.0")
        self.assertNotEqual(real_form["z2_maxtemp"], "0.0")

    def test_unknown_zone_field_from_get_raises_rather_than_silently_dropping(self):
        """NEGATIVE TEST for the dynamic-derivation requirement: if GET
        /api/zones ever reports a zone field this module has no mapping for
        (e.g. a field a concurrently-landing firmware change adds), building
        the POST body must FAIL LOUDLY, not silently omit the field (which
        would zero it on the board for most zone fields)."""
        current = _sample_get_response()
        current["zones"][0]["brand_new_field_from_the_future"] = 1.0
        with self.assertRaises(zh.ZonesHttpUnknownFieldError) as ctx:
            zh.build_post_body(current, {"name": "p", "zones": []})
        self.assertIn("brand_new_field_from_the_future", str(ctx.exception))

    def test_unknown_top_level_field_from_get_raises(self):
        current = _sample_get_response()
        current["brand_new_top_level_field"] = 1
        with self.assertRaises(zh.ZonesHttpUnknownFieldError):
            zh.build_post_body(current, {"name": "p", "zones": []})

    def test_missing_timing_profiles_raises(self):
        current = _sample_get_response()
        current["timing_profiles"] = []
        with self.assertRaises(zh.ZonesHttpError):
            zh.build_post_body(current, {"name": "p", "zones": []})

    def test_relay_names_never_posted(self):
        """relay<N>_name uses the OPPOSITE (omit-preserves) convention --
        this module must never send it at all, which is what preserves it."""
        current = _sample_get_response()
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        for k in form:
            self.assertFalse(k.startswith("relay") and k.endswith("_name"), k)

    def test_continue_on_zone_trip_encoded_as_1_or_0_not_python_bool(self):
        current = _sample_get_response()
        current["continue_on_zone_trip"] = True
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["continue_on_zone_trip"], "1")

    # ---- PID_EXPANSION_PLAN.md Phase 2/4/5 fields (fuzzy_strength_pct,
    # coupling_c%u, settings_source): added to zones_http.c 2026-08-30, the
    # producer/consumer gap this task exists to close. ----

    def test_every_currently_emitted_field_round_trips_without_unknown_field_refusal(self):
        """The mandatory 'GET payload containing every currently-emitted
        field builds without refusal, values unchanged' check -- covers
        fuzzy_strength_pct, all three coupling_c%u cells and
        settings_source in one pass, not just the mapping's existence."""
        current = _sample_get_response(n_zones=3)
        current["zones"][1]["fuzzy_strength_pct"] = 37.5
        current["zones"][1]["coupling_c0"] = 0.0     # diagonal for zone 0, off-diag for zone 1
        current["zones"][1]["coupling_c2"] = 1.25
        current["zones"][1]["settings_source"] = 0
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["z1_fuzzy_strength"], repr(37.5))
        self.assertEqual(form["z1_coupling_c0"], repr(0.0))
        self.assertEqual(form["z1_coupling_c2"], repr(1.25))
        self.assertEqual(form["z1_settings_source"], "0")
        # settings_source == 0 is a REAL distinct value (a chain link to
        # zone 0), not the "not set" sentinel -- must round-trip as "0",
        # never coerced to something else or dropped as falsy.
        self.assertIn("z1_settings_source", form)

    def test_settings_source_custom_sentinel_round_trips_as_255(self):
        current = _sample_get_response()
        current["zones"][0]["settings_source"] = 0xFF
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["z0_settings_source"], "255")

    def test_coupling_diagonal_never_posted_nonzero(self):
        """Semantics test: the diagonal (j == the zone's own index) must
        never be posted nonzero -- zones_http.c's parse_zone_fields() force-
        ranges it to exactly [0,0] and a client that posted a stray nonzero
        value there would simply be refused by the board, but this module
        is supposed to catch it before the request ever goes out."""
        current = _sample_get_response(n_zones=3)
        # Diagonal correctly 0 -- must round-trip fine.
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["z0_coupling_c0"], repr(0.0))
        self.assertEqual(form["z1_coupling_c1"], repr(0.0))
        self.assertEqual(form["z2_coupling_c2"], repr(0.0))

    def test_coupling_diagonal_nonzero_is_refused_BREAK_PROOF(self):
        """NEGATIVE TEST: corrupt zone 1's own diagonal cell (coupling_c1)
        to a nonzero value and prove build_post_body() refuses to post it,
        rather than silently forwarding a value the firmware would reject
        anyway (or, if the firmware's own guard ever regressed, silently
        accepting)."""
        current = _sample_get_response(n_zones=3)
        current["zones"][1]["coupling_c1"] = 2.5  # zone 1's own diagonal cell
        with self.assertRaises(zh.ZonesHttpError) as ctx:
            zh.build_post_body(current, {"name": "p", "zones": []})
        self.assertIn("coupling_c1", str(ctx.exception))
        self.assertIn("diagonal", str(ctx.exception))

    def test_control_mode_3_pid_fuzzy_accepted(self):
        """control_mode now accepts ZONE_CONTROL_MODE_PID_FUZZY (3) -- confirm
        this module's int-field encoding does not clamp/reject it."""
        current = _sample_get_response()
        current["zones"][0]["control_mode"] = 3
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["z0_mode"], "3")

    def test_unmapped_field_guard_still_refuses_BREAK_PROOF(self):
        """NEGATIVE TEST for the guard itself, run again post-fix: an
        UNEXPECTED field (not fuzzy_strength_pct/coupling_c%u/
        settings_source, and not anything else this module knows) must
        still raise. This is the exact guard the aborted hardware run hit,
        and the fix above must not have weakened it into silence."""
        current = _sample_get_response()
        current["zones"][0]["totally_unmapped_field_xyz"] = 1.0
        with self.assertRaises(zh.ZonesHttpUnknownFieldError) as ctx:
            zh.build_post_body(current, {"name": "p", "zones": []})
        self.assertIn("totally_unmapped_field_xyz", str(ctx.exception))


class PostZonesTest(unittest.TestCase):
    def test_sends_form_encoded_body_and_returns_ok_text(self):
        with unittest.mock.patch.object(zh.urllib.request, "urlopen",
                                         return_value=_fake_response(b"ok")):
            result = zh.post_zones("kiln.local", "thermo_count=3")
        self.assertEqual(result, "ok")

    def test_surfaces_400_rejection_reason_as_plain_text(self):
        err = urllib.error.HTTPError(
            "http://x/api/zones", 400, "Bad Request", hdrs=None,
            fp=io.BytesIO(b"zone relay_mask references an unconfigured relay"))
        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(zh.ZonesHttpError) as ctx:
                zh.post_zones("kiln.local", "bogus=1")
        self.assertEqual(ctx.exception.status, 400)
        self.assertIn("unconfigured relay", ctx.exception.detail)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(zh.ZonesHttpError):
                zh.post_zones("192.0.2.1", "x=1")


class ApplyZonePresetTest(unittest.TestCase):
    def _mock_get_then_post_then_get(self, first_get: dict, post_body: bytes, second_get: dict):
        calls = {"n": 0}

        def fake_urlopen(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(json.dumps(first_get).encode())
            if calls["n"] == 2:
                return _fake_response(post_body)
            return _fake_response(json.dumps(second_get).encode())

        return fake_urlopen, calls

    def test_verify_true_passes_when_readback_matches(self):
        current = _sample_get_response()
        preset = {
            "name": "p", "zones": [{"index": 0, "relay_mask": 1, "control_mode": 0, "cal_offset_c": 0.0,
                                     "pid_kp": 0.0, "pid_ki": 0.0, "pid_kd": 0.0, "max_ramp_c_per_hr": 0.0,
                                     "max_temp_c": 42.0, "min_temp_c": 0.0}],
        }
        after = _sample_get_response()
        after["zones"][0].update(
            relay_mask=1, control_mode=0, cal_offset_c=0.0, pid_kp=0.0, pid_ki=0.0, pid_kd=0.0,
            max_ramp_c_per_hr=0.0, max_temp_c=42.0, min_temp_c=0.0)
        fake_urlopen, _ = self._mock_get_then_post_then_get(current, b"ok", after)
        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = zh.apply_zone_preset("kiln.local", preset)
        self.assertTrue(result.ok, result.describe())
        self.assertEqual(result.mismatches, [])

    def test_verify_true_FAILS_when_readback_disagrees(self):
        """NEGATIVE TEST for the read-back-verification contract: a board
        that ACKs the POST but doesn't actually apply the value (the
        documented failure class -- "POST /api/safety/commissioning
        returned {"ok":true} for writes that never landed") must come back
        as ok=False with the specific mismatch, not a silent success."""
        current = _sample_get_response()
        preset = {
            "name": "p", "zones": [{"index": 0, "relay_mask": 1, "control_mode": 0, "cal_offset_c": 0.0,
                                     "pid_kp": 0.0, "pid_ki": 0.0, "pid_kd": 0.0, "max_ramp_c_per_hr": 0.0,
                                     "max_temp_c": 42.0, "min_temp_c": 0.0}],
        }
        after = _sample_get_response()  # max_temp_c still 80.0 -- write did not land
        fake_urlopen, _ = self._mock_get_then_post_then_get(current, b"ok", after)
        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = zh.apply_zone_preset("kiln.local", preset)
        self.assertFalse(result.ok)
        self.assertTrue(any("max_temp_c" in m for m in result.mismatches), result.mismatches)
        self.assertIn("FAILED" if False else "did NOT verify", result.describe())

    def test_non_ok_post_body_raises(self):
        current = _sample_get_response()
        fake_urlopen, _ = self._mock_get_then_post_then_get(current, b"not ok somehow", current)
        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(zh.ZonesHttpError):
                zh.apply_zone_preset("kiln.local", {"name": "p", "zones": []})

    def test_verify_false_skips_readback(self):
        current = _sample_get_response()
        calls = {"n": 0}

        def fake_urlopen(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(json.dumps(current).encode())
            return _fake_response(b"ok")

        with unittest.mock.patch.object(zh.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = zh.apply_zone_preset("kiln.local", {"name": "p", "zones": []}, verify=False)
        self.assertEqual(calls["n"], 2)  # GET + POST, no second GET
        self.assertTrue(result.ok)


if __name__ == "__main__":
    unittest.main()
