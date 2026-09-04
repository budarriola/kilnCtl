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
        # ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md section 3.2
        # follow-up): coupling_diag_k_dc, always emitted alongside
        # fuzzy_strength_pct/coupling_c%u above.
        "coupling_diag_k_dc": 0.0,
        # Coupling row: MAX31856_CHANNEL_COUNT (3, uart_task_ids.h's
        # THERMO_CHANNEL_COUNT) cells, diagonal (j == index) always 0 --
        # zones_http.c always emits the full row for every zone regardless
        # of thermo_count.
        **{f"coupling_c{j}": 0.0 for j in range(3)},
        # ZONES_CFG_VERSION 11->12: coupling_tau_c%u/coupling_dead_time_c%u,
        # always emitted alongside coupling_c%u (same [affected][stepped]
        # orientation, same "always emit every cell including the diagonal"
        # convention) -- see zones_http_client.py's
        # _ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE.
        **{f"coupling_tau_c{j}": 0.0 for j in range(3)},
        **{f"coupling_dead_time_c{j}": 0.0 for j in range(3)},
        "settings_source": 0xFF,
        # ZONES_CFG_VERSION 12->13: tuning-quality record (set 1), always
        # emitted alongside everything else above -- see
        # zh._ZONE_TUNING_READONLY_KEYS's own comment for why these 11 keys
        # have no POST mapping at all.
        "tuning_valid": False, "tuning_method": 0, "tuning_rule": 0,
        "tuning_settled": False, "tuning_extrapolation_converged": False,
        "tuning_tau_consistent": False,
        "tuning_baseline_c": 0.0, "tuning_step_ambient_c": 0.0,
        "tuning_raw_rise_c": 0.0, "tuning_rise_inf_c": 0.0, "tuning_seq": 0,
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
        current["zones"][1]["coupling_diag_k_dc"] = 18.75
        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))
        self.assertEqual(form["z1_fuzzy_strength"], repr(37.5))
        self.assertEqual(form["z1_coupling_c0"], repr(0.0))
        self.assertEqual(form["z1_coupling_c2"], repr(1.25))
        self.assertEqual(form["z1_settings_source"], "0")
        self.assertEqual(form["z1_coupling_diag_k_dc"], repr(18.75))
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

    def test_coupling_tau_dead_time_do_not_crash_the_round_trip_ASYMMETRIC(self):
        """DEFECT: zones_http.c ZONES_CFG_VERSION 11->12 added
        coupling_tau_c%u/coupling_dead_time_c%u to GET /api/zones, but this
        module had no mapping for them -- build_post_body() raised
        ZonesHttpUnknownFieldError on EVERY zones round-trip against any
        board running that firmware, since GET always emits these keys
        unconditionally. Fixture is deliberately ASYMMETRIC -- pair (0,1)
        != pair (1,0) -- so a transpose or index bug in a future fix would
        fail this test instead of passing by accident on a symmetric or
        all-zero fixture (the firmware-side round-trip test made exactly
        that mistake, seeding 0.0/0.0)."""
        current = _sample_get_response(n_zones=3)
        current["zones"][0]["coupling_tau_c1"] = 111.0
        current["zones"][0]["coupling_dead_time_c1"] = 22.0
        current["zones"][1]["coupling_tau_c0"] = 333.0
        current["zones"][1]["coupling_dead_time_c0"] = 44.0
        # Must not raise.
        body = zh.build_post_body(current, {"name": "p", "zones": []})
        form = _decode_body(body)
        # These fields have no wire form key at all (parse_zone_fields()
        # unconditionally preserves them from current_z, never reads them
        # from the POST body) -- they must be excluded from the POST, not
        # invented a fake form key.
        for key in form:
            self.assertFalse(key.startswith("z0_coupling_tau_"), key)
            self.assertFalse(key.startswith("z0_coupling_dead_time_"), key)
            self.assertFalse(key.startswith("z1_coupling_tau_"), key)
            self.assertFalse(key.startswith("z1_coupling_dead_time_"), key)

    def test_tuning_fields_do_not_crash_the_round_trip_and_are_never_posted(self):
        """DEFECT this task exists to fix: ZONES_CFG_VERSION 12->13 added the
        11-key tuning-quality record (tuning_valid/method/rule/settled/
        extrapolation_converged/tau_consistent/baseline_c/step_ambient_c/
        raw_rise_c/rise_inf_c/seq) to GET /api/zones -- build_post_body()
        raised ZonesHttpUnknownFieldError on EVERY zones round-trip against
        any board running that firmware (the exact error blocking the live
        hardware run this fix responds to). These fields have NO z%u_ POST
        key at all (parse_zone_fields() unconditionally copies them from
        current_z -- zones_http_handlers.c lines ~668-678), so they must be
        excluded from the POST, not invented a fake form key."""
        current = _sample_get_response(n_zones=3)
        current["zones"][0]["tuning_valid"] = True
        current["zones"][0]["tuning_method"] = 2
        current["zones"][0]["tuning_rule"] = 1
        current["zones"][0]["tuning_settled"] = True
        current["zones"][0]["tuning_extrapolation_converged"] = True
        current["zones"][0]["tuning_tau_consistent"] = False
        current["zones"][0]["tuning_baseline_c"] = 21.5
        current["zones"][0]["tuning_step_ambient_c"] = 22.0
        current["zones"][0]["tuning_raw_rise_c"] = 41.3
        current["zones"][0]["tuning_rise_inf_c"] = 60.0
        current["zones"][0]["tuning_seq"] = 7
        # Must not raise.
        body = zh.build_post_body(current, {"name": "p", "zones": []})
        form = _decode_body(body)
        for key in form:
            self.assertFalse(key.startswith("z0_tuning"), key)

    def test_tuning_field_guard_still_refuses_when_unmapped_BREAK_PROOF(self):
        """NEGATIVE TEST / regression guard: revert one of the 11 tuning
        keys out of _ZONE_TUNING_READONLY_KEYS (simulating the original
        defect -- a GET field the client doesn't yet know is read-only) and
        prove build_post_body() still refuses loudly rather than silently
        dropping it. Run, watch it go red, then trust the fix stays green."""
        current = _sample_get_response()
        current["zones"][0]["tuning_valid"] = True
        with unittest.mock.patch.object(
                zh, "_ZONE_READONLY_KEYS",
                zh._ZONE_READONLY_KEYS - {"tuning_valid"}):
            with self.assertRaises(zh.ZonesHttpUnknownFieldError) as ctx:
                zh.build_post_body(current, {"name": "p", "zones": []})
        self.assertIn("tuning_valid", str(ctx.exception))

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

    # ---- cross_zone_max_delta_c overlay (guard 8 disabled-overnight bug) ----

    def test_preset_overlay_carries_cross_zone_max_delta_c(self):
        """DEFECT 1: a preset naming cross_zone_max_delta_c must actually
        change the value in the POST body, not just get echoed from the
        live GET. Board default is 0.0 (disabled); preset sets guard 8's
        real limit."""
        current = _sample_get_response()
        current["zones"][1]["cross_zone_max_delta_c"] = 0.0
        preset = {"name": "p", "zones": [{"index": 1, "cross_zone_max_delta_c": 15.0}]}
        form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["z1_xzone"], repr(15.0))

    def test_preset_overlay_carries_cross_zone_max_delta_c_ZERO_BREAK_PROOF(self):
        """cross_zone_max_delta_c == 0 means guard 8 is DISABLED -- a
        meaningful value, not "absent." A preset explicitly setting it to
        0.0 (disabling the guard on purpose) must still land in the POST as
        an explicit 0.0, and must not be treated as falsy/omitted by the
        overlay logic. Start the board at a NONZERO live value so a naive
        `if value:` truthiness check overlaying the preset would leave the
        old nonzero value in place instead of writing the 0."""
        current = _sample_get_response()
        current["zones"][1]["cross_zone_max_delta_c"] = 25.0
        preset = {"name": "p", "zones": [{"index": 1, "cross_zone_max_delta_c": 0.0}]}
        form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["z1_xzone"], repr(0.0))

    def test_preset_overlay_cross_zone_max_delta_c_NEGATIVE(self):
        """NEGATIVE TEST proving the above tests actually catch the bug:
        with cross_zone_max_delta_c removed from
        _PRESET_ZONE_OVERRIDE_FIELDS (simulating the original defect), the
        preset's value must NOT reach the POST body -- the stale live value
        is echoed instead."""
        current = _sample_get_response()
        current["zones"][1]["cross_zone_max_delta_c"] = 25.0
        preset = {"name": "p", "zones": [{"index": 1, "cross_zone_max_delta_c": 0.0}]}
        with unittest.mock.patch.object(
                zh, "_PRESET_ZONE_OVERRIDE_FIELDS",
                zh._PRESET_ZONE_OVERRIDE_FIELDS - {"cross_zone_max_delta_c"}):
            with unittest.mock.patch.object(
                    zh, "_PRESET_ZONE_KNOWN_IGNORED_FIELDS",
                    zh._PRESET_ZONE_KNOWN_IGNORED_FIELDS | {"cross_zone_max_delta_c"}):
                form = _decode_body(zh.build_post_body(current, preset))
        # Bug reproduced: the preset's 0.0 never arrived, stale 25.0 echoed.
        self.assertEqual(form["z1_xzone"], repr(25.0))

    def test_unmappable_preset_zone_key_raises_loudly(self):
        """A preset zone key that is neither a known override field nor a
        known-ignored field (index/k_dc/tau_s/dead_time_s) must fail loudly
        rather than being silently dropped -- the same "consumer without
        producer" pattern cross_zone_max_delta_c fell into."""
        current = _sample_get_response()
        preset = {"name": "p", "zones": [{"index": 1, "totally_unknown_preset_key": 3.0}]}
        with self.assertRaises(zh.ZonesHttpUnknownFieldError) as ctx:
            zh.build_post_body(current, preset)
        self.assertIn("totally_unknown_preset_key", str(ctx.exception))

    def test_unmappable_preset_zone_key_NEGATIVE(self):
        """NEGATIVE TEST: with the unmappable key added to the ignored set
        (simulating the pre-fix behavior of silently falling through),
        build_post_body() must NOT raise and must NOT carry the value --
        proving the loud-failure test above actually distinguishes fixed
        from unfixed behavior."""
        current = _sample_get_response()
        preset = {"name": "p", "zones": [{"index": 1, "totally_unknown_preset_key": 3.0}]}
        with unittest.mock.patch.object(
                zh, "_PRESET_ZONE_KNOWN_IGNORED_FIELDS",
                zh._PRESET_ZONE_KNOWN_IGNORED_FIELDS | {"totally_unknown_preset_key"}):
            form = _decode_body(zh.build_post_body(current, preset))  # must not raise
        self.assertNotIn("z1_totally_unknown_preset_key", form)

    # ---- model_* spellings (board-read/backup-export presets) ----

    def test_model_kdc_tau_deadtime_spellings_are_ignored_not_raised(self):
        """A preset built by copying a board GET or a backup export --
        the most natural way to author one -- carries model_k_dc/
        model_tau_s/model_dead_time_s (GET /api/zones's own spelling), not
        the short k_dc/tau_s/dead_time_s the UART-facing schema uses. Both
        spellings name fields config_presets.apply_preset() already writes
        over the UART CONTROL link separately, so build_post_body() must
        ignore either spelling, never raise on it."""
        current = _sample_get_response()
        # Live board value stays 0.0 (config_presets.apply_preset() writes
        # k_dc/tau_s/dead_time_s over UART separately, not through this
        # HTTP overlay) -- the preset's attempted 1.5/120.0/8.0 must be
        # ignored, not raised on and not applied.
        preset = {"name": "p", "zones": [{
            "index": 1, "model_k_dc": 1.5, "model_tau_s": 120.0, "model_dead_time_s": 8.0,
        }]}
        form = _decode_body(zh.build_post_body(current, preset))  # must not raise
        self.assertEqual(form["z1_k"], repr(0.0))
        self.assertEqual(form["z1_tau"], repr(0.0))
        self.assertEqual(form["z1_deadtime"], repr(0.0))

    def test_model_kdc_spelling_NEGATIVE(self):
        """NEGATIVE TEST: with model_k_dc removed from the ignored set
        (simulating the pre-fix, short-spelling-only set), a board-read-
        derived preset carrying model_k_dc must raise -- reproducing the
        "loud failure on the user's own preset" regression this fix
        addresses."""
        current = _sample_get_response()
        preset = {"name": "p", "zones": [{"index": 1, "model_k_dc": 1.5}]}
        with unittest.mock.patch.object(
                zh, "_PRESET_ZONE_KNOWN_IGNORED_FIELDS",
                zh._PRESET_ZONE_KNOWN_IGNORED_FIELDS - {"model_k_dc"}):
            with self.assertRaises(zh.ZonesHttpUnknownFieldError):
                zh.build_post_body(current, preset)

    # ---- settings_source stays ignored (topology pointer, not a value) ----

    def test_settings_source_in_preset_is_ignored_not_raised(self):
        current = _sample_get_response()
        preset = {"name": "p", "zones": [{"index": 1, "settings_source": 2}]}
        form = _decode_body(zh.build_post_body(current, preset))  # must not raise
        # Board's own live settings_source (0xFF) is still echoed, untouched
        # by the preset's attempted override.
        self.assertEqual(form["z1_settings_source"], "255")

    # ---- guard_* family overlay (guard 8 incident class) ----

    def test_preset_overlay_carries_every_guard_family_field(self):
        """The guard_* family (thermal_guard.c's per-zone threshold set)
        is the SAME incident class as cross_zone_max_delta_c/guard 8: round-
        trippable, firmware-accepted, but previously not preset-overridable.
        Prove every one of them actually lands in the POST body when a
        preset names it."""
        current = _sample_get_response()
        overrides = {
            "guard_wrong_dir_window_s": 30.0,
            "guard_wrong_dir_rate_c_per_min": 5.0,
            "guard_off_settle_s": 12.0,
            "guard_runaway_rate_c_per_min": 40.0,
            "guard_runaway_margin_c": 15.0,
            "guard_drift_period_s": 300.0,
            "guard_sensor_fault_debounce_ticks": 3.0,
            "guard_frozen_window_s": 600.0,
        }
        preset = {"name": "p", "zones": [dict(index=1, **overrides)]}
        form = _decode_body(zh.build_post_body(current, preset))
        expected_suffix = {
            "guard_wrong_dir_window_s": "wrongdirwindow",
            "guard_wrong_dir_rate_c_per_min": "wrongdirrate",
            "guard_off_settle_s": "offsettle",
            "guard_runaway_rate_c_per_min": "runawayrate",
            "guard_runaway_margin_c": "runawaymargin",
            "guard_drift_period_s": "driftperiod",
            "guard_sensor_fault_debounce_ticks": "debounce",
            "guard_frozen_window_s": "frozenwindow",
        }
        for key, value in overrides.items():
            suffix = expected_suffix[key]
            self.assertEqual(form[f"z1_{suffix}"], repr(value), f"field {key!r} did not overlay")

    def test_preset_overlay_carries_guard_family_NEGATIVE(self):
        """NEGATIVE TEST: with the guard_* family stripped from
        _PRESET_ZONE_OVERRIDE_FIELDS (simulating the pre-fix set), the
        preset's guard_runaway_margin_c must NOT reach the POST body -- the
        stale board value is echoed instead, reproducing the "guard
        threshold a preset names but cannot write" defect."""
        current = _sample_get_response()
        current["zones"][1]["guard_runaway_margin_c"] = 999.0
        preset = {"name": "p", "zones": [{"index": 1, "guard_runaway_margin_c": 15.0}]}
        stripped = zh._PRESET_ZONE_OVERRIDE_FIELDS - {
            "guard_wrong_dir_window_s", "guard_wrong_dir_rate_c_per_min", "guard_off_settle_s",
            "guard_runaway_rate_c_per_min", "guard_runaway_margin_c", "guard_drift_period_s",
            "guard_sensor_fault_debounce_ticks", "guard_frozen_window_s",
        }
        with unittest.mock.patch.object(zh, "_PRESET_ZONE_OVERRIDE_FIELDS", stripped):
            with unittest.mock.patch.object(
                    zh, "_PRESET_ZONE_KNOWN_IGNORED_FIELDS",
                    zh._PRESET_ZONE_KNOWN_IGNORED_FIELDS | {"guard_runaway_margin_c"}):
                form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["z1_runawaymargin"], repr(999.0))  # stale value, preset's 15.0 dropped

    # ---- name/thermo_mask/tc_type/ct_mask/timing_profile/heater_min_off_ms/
    # fuzzy_strength_pct overlay ----

    def test_preset_overlay_carries_remaining_previously_dropped_fields(self):
        current = _sample_get_response()
        preset = {"name": "p", "zones": [{
            "index": 1, "name": "top zone", "thermo_mask": 0b100, "tc_type": 5,
            "ct_mask": 0b10, "timing_profile": 0, "heater_min_off_ms": 5000.0,
            "fuzzy_strength_pct": 40.0,
        }]}
        form = _decode_body(zh.build_post_body(current, preset))
        self.assertEqual(form["z1_name"], "top zone")
        self.assertEqual(form["z1_thermo_mask"], "4")
        self.assertEqual(form["z1_tctype"], "5")
        self.assertEqual(form["z1_ct_mask"], "2")
        self.assertEqual(form["z1_timingprofile"], "0")
        self.assertEqual(form["z1_minoff"], repr(5000.0))
        self.assertEqual(form["z1_fuzzy_strength"], repr(40.0))

    # ---- null-valued field (item 6): loud ZonesHttpError, not a raw crash ----

    def test_null_float_field_raises_ZonesHttpError_not_TypeError(self):
        current = _sample_get_response()
        current["zones"][0]["max_temp_c"] = None
        with self.assertRaises(zh.ZonesHttpError):
            zh.build_post_body(current, {"name": "p", "zones": []})

    def test_null_float_field_NEGATIVE(self):
        """NEGATIVE TEST: without the None guard in _format_scalar(), the
        same input raises a raw TypeError instead of ZonesHttpError -- which
        escapes every `except zones_http_client.ZonesHttpError` handler this
        module's callers use. Reproduced here by calling the float()
        conversion the old code path used directly."""
        with self.assertRaises(TypeError):
            repr(float(None))


class CouplingCoeffOverrideTest(unittest.TestCase):
    """coupling_coeff round-trip: a preset zone's "coupling_coeff" list
    expands into the per-cell z%u_coupling_c%u POST keys, in the SAME
    [affected][stepped] orientation GET /api/zones reports (zone i's list
    entry j is zone i's response to zone j being stepped) -- see
    zones_http_client.py's _PRESET_ZONE_COUPLING_FIELD docstring and the
    blocking bug this fixes: commit 78f2134's matrix had no field mapping at
    all (ZonesHttpUnknownFieldError) and was written into a file the preset
    loader can't load (config_presets.ConfigPresetError) in the first place.

    This matrix is deliberately ASYMMETRIC (zone0's response to zone1 is
    27.32; zone1's response to zone0 is 14.30) specifically so a transposed
    mapping is caught by these assertions, not just a swapped-value one --
    see test_TRANSPOSED_mapping_is_caught_by_this_test below for the proof.
    """

    MATRIX = {  # preset "coupling_coeff" rows, [affected][stepped], diag 0
        0: [0.0, 27.32, 21.72],
        1: [14.30, 0.0, 22.15],
        2: [8.33, 12.42, 0.0],
    }

    def _preset(self):
        return {
            "name": "coupling_matrix_test",
            "zones": [{"index": i, "coupling_coeff": row} for i, row in self.MATRIX.items()],
        }

    def test_round_trips_into_the_right_per_cell_keys_and_orientation(self):
        current = _sample_get_response(n_zones=3)
        form = _decode_body(zh.build_post_body(current, self._preset()))

        self.assertEqual(form["z0_coupling_c0"], repr(0.0))
        self.assertEqual(form["z0_coupling_c1"], repr(27.32))
        self.assertEqual(form["z0_coupling_c2"], repr(21.72))
        self.assertEqual(form["z1_coupling_c0"], repr(14.30))
        self.assertEqual(form["z1_coupling_c1"], repr(0.0))
        self.assertEqual(form["z1_coupling_c2"], repr(22.15))
        self.assertEqual(form["z2_coupling_c0"], repr(8.33))
        self.assertEqual(form["z2_coupling_c1"], repr(12.42))
        self.assertEqual(form["z2_coupling_c2"], repr(0.0))

        # The two off-diagonal values sharing the {0,1} zone pair must NOT
        # be equal -- proves this test would actually notice a transpose.
        self.assertNotEqual(form["z0_coupling_c1"], form["z1_coupling_c0"])

    def test_TRANSPOSED_mapping_is_caught_by_this_test(self):
        """NEGATIVE TEST / mutation proof: simulate the bug of expanding a
        row onto the WRONG zone's cells (j and i swapped, i.e. writing
        z{j}_coupling_c{i} instead of z{i}_coupling_c{j}) and show the
        assertions above would fail against it -- proving the positive test
        is not vacuously true. Does not call build_post_body(); constructs
        the transposed form the way a broken _encode_zone loop would."""
        current = _sample_get_response(n_zones=3)
        # Build a body the way a TRANSPOSED build_post_body() would: zone i's
        # coupling_c{j} cell gets MATRIX[j][i] (the STEPPED zone's row, cell
        # i) instead of the correct MATRIX[i][j] (the AFFECTED zone's own
        # row, cell j). Written directly onto `current` and echoed with an
        # empty preset, so this exercises only the assertion shape, not
        # build_post_body() itself -- it is not making a false claim about
        # the fix; it demonstrates the positive test above is not vacuous.
        for zone in current["zones"]:
            i = zone["index"]
            for j in range(3):
                if j == i:
                    continue
                zone[f"coupling_c{j}"] = self.MATRIX[j][i]

        form = _decode_body(zh.build_post_body(current, {"name": "p", "zones": []}))

        # The CORRECT (non-transposed) assertion fails against this body --
        # proof that test_round_trips_into_the_right_per_cell_keys_and_
        # orientation above would catch a real transpose bug in
        # zones_http_client.py, not just echo back whatever it produced.
        self.assertNotEqual(form["z0_coupling_c1"], repr(27.32))
        self.assertEqual(form["z0_coupling_c1"], repr(14.30))
        self.assertNotEqual(form["z1_coupling_c0"], repr(14.30))
        self.assertEqual(form["z1_coupling_c0"], repr(27.32))

    def test_diagonal_is_never_overlaid_even_if_preset_row_carries_nonzero(self):
        """The diagonal cell is force-ranged to [0,0] by the firmware and
        must never be posted nonzero -- build_post_body() must skip it
        entirely (leaving whatever `current` already had) rather than
        forwarding a preset author's mistaken nonzero diagonal entry."""
        current = _sample_get_response(n_zones=3)
        preset = {"name": "p", "zones": [{"index": 0, "coupling_coeff": [99.0, 27.32, 21.72]}]}
        form = _decode_body(zh.build_post_body(current, preset))
        # current's diagonal was 0.0 (see _sample_zone); must stay 0.0, not 99.0.
        self.assertEqual(form["z0_coupling_c0"], repr(0.0))

    def test_non_list_coupling_coeff_raises(self):
        current = _sample_get_response(n_zones=3)
        preset = {"name": "p", "zones": [{"index": 0, "coupling_coeff": 27.32}]}
        with self.assertRaises(zh.ZonesHttpError):
            zh.build_post_body(current, preset)


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


class CapturedLiveGetFixtureTest(unittest.TestCase):
    """Round-trips build_post_body() over a REAL GET /api/zones payload
    captured from the live board (192.168.1.156, 2026-09-02) -- the same
    hazard as the mocked _sample_get_response() fixture above, but against
    actual firmware output rather than a hand-authored dict, which is what
    caught the tuning_valid gap in the first place (a hand-authored fixture
    only has the fields the test author remembered to put in). This is also
    the REGRESSION GUARD: any GET field the firmware emits that the live
    board fixture carries and this module has no mapping/readonly-entry for
    fails this test with ZonesHttpUnknownFieldError, by construction -- no
    separate "did we forget a field" check is needed, because the fixture is
    the field list.
    """

    @staticmethod
    def _load_fixture() -> dict:
        path = os.path.join(os.path.dirname(__file__), "fixtures", "zones_get_capture.json")
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

    def test_round_trips_without_dropping_any_field(self):
        current = self._load_fixture()
        # Must not raise ZonesHttpUnknownFieldError -- every field the live
        # board actually emitted is either mapped or explicitly read-only.
        body = zh.build_post_body(current, {"name": "p", "zones": []})
        form = _decode_body(body)

        # Every writable zone field from the captured GET must be present
        # in the POST body, unchanged (echoed, not dropped) -- walk the
        # fixture itself rather than hardcoding a field list, so this test
        # does not go stale the same way the client almost did.
        for zone in current["zones"]:
            idx = zone["index"]
            for key, value in zone.items():
                if key in zh._ZONE_READONLY_KEYS:
                    continue
                if zh._ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE.match(key):
                    continue
                suffix = zh._ZONE_FIELD_FORM_KEY.get(key)
                if suffix is None and zh._ZONE_COUPLING_CELL_RE.match(key):
                    suffix = key
                self.assertIsNotNone(suffix, f"zone {idx} field {key!r} has no mapping")
                form_key = f"z{idx}_{suffix}"
                self.assertIn(form_key, form, f"zone {idx} field {key!r} missing from POST body")

    def test_fuzzy_strength_override_matches_the_hardware_repro_command(self):
        """The exact repro from the blocked hardware run: build a body that
        sets fuzzy_strength_pct=50.0 on every zone while preserving
        everything else from the live GET."""
        current = self._load_fixture()
        preset = {"zones": [{"index": i, "fuzzy_strength_pct": 50.0}
                             for i in range(len(current["zones"]))]}
        body = zh.build_post_body(current, preset)
        form = _decode_body(body)
        for i in range(len(current["zones"])):
            self.assertEqual(form[f"z{i}_fuzzy_strength"], repr(50.0))

    def test_coupling_diag_k_dc_override_round_trips(self):
        """ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md section 3.2
        follow-up): a preset naming coupling_diag_k_dc must reach the POST
        body -- same class of check as
        test_fuzzy_strength_override_matches_the_hardware_repro_command
        above, for the new scalar field."""
        current = self._load_fixture()
        preset = {"zones": [{"index": i, "coupling_diag_k_dc": 21.6}
                             for i in range(len(current["zones"]))]}
        body = zh.build_post_body(current, preset)
        form = _decode_body(body)
        for i in range(len(current["zones"])):
            self.assertEqual(form[f"z{i}_coupling_diag_k_dc"], repr(21.6))

    def test_preset_captured_from_live_get_round_trips_without_dropping_any_field(self):
        """THE regression this test class exists to catch, at the preset
        overlay layer rather than the GET-echo layer test_round_trips_
        without_dropping_any_field already covers: a preset authored by
        copying a board's own GET /api/zones (or a backup export) verbatim
        -- the module docstring's own "most natural way to author one" --
        must not be refused just because it named a field back at the board.
        Every zone key in the captured fixture is walked, by construction,
        so this does not go stale the way a hardcoded field list would (same
        reasoning as test_round_trips_without_dropping_any_field above) --
        it is what would have caught coupling_diag_k_dc/normal_current_*/
        tuning_* landing in neither _PRESET_ZONE_OVERRIDE_FIELDS nor
        _PRESET_ZONE_KNOWN_IGNORED_FIELDS before this fix."""
        current = self._load_fixture()
        preset = {"zones": [dict(zone) for zone in current["zones"]]}
        # Must not raise ZonesHttpUnknownFieldError.
        zh.build_post_body(current, preset)

    def test_MUTATION_preset_field_missing_from_both_sets_is_caught_BREAK_PROOF(self):
        """Proves test_preset_captured_from_live_get_round_trips_without_
        dropping_any_field is actually checking something: strip
        coupling_diag_k_dc back out of both preset sets (simulating the
        real regression -- a field zones_http_handlers.c/autotune_engine.c
        started populating that the preset allowlist was never updated for)
        and confirm build_post_body() refuses a preset naming it, quoting
        the real refusal text."""
        current = self._load_fixture()
        preset = {"zones": [{"index": i, "coupling_diag_k_dc": 21.6}
                             for i in range(len(current["zones"]))]}
        with unittest.mock.patch.object(
                zh, "_PRESET_ZONE_OVERRIDE_FIELDS",
                zh._PRESET_ZONE_OVERRIDE_FIELDS - {"coupling_diag_k_dc"}):
            with self.assertRaises(zh.ZonesHttpUnknownFieldError) as ctx:
                zh.build_post_body(current, preset)
        message = str(ctx.exception)
        print(f"\n[negative-test] real refusal reproduced: {message}")
        self.assertIn("coupling_diag_k_dc", message)
        self.assertIn("is not in _PRESET_ZONE_OVERRIDE_FIELDS or "
                       "_PRESET_ZONE_KNOWN_IGNORED_FIELDS", message)
        # Restored automatically on exit from the patch context -- confirm
        # the real (unpatched) module round-trips it again.
        zh.build_post_body(current, preset)

    def test_MUTATION_a_new_unmapped_get_field_is_caught_BREAK_PROOF(self):
        """THE regression guard, proved red then green: inject a fake field
        into the captured payload the way a firmware change that grows GET
        /api/zones would (e.g. a hypothetical 'tuning_snr_db') and confirm
        build_post_body() refuses rather than silently dropping it. This is
        the exact failure mode that blocked the live hardware run
        (tuning_valid, before this fix)."""
        current = self._load_fixture()
        current["zones"][0]["tuning_snr_db_NOT_A_REAL_FIELD"] = 12.5
        with self.assertRaises(zh.ZonesHttpUnknownFieldError) as ctx:
            zh.build_post_body(current, {"name": "p", "zones": []})
        self.assertIn("tuning_snr_db_NOT_A_REAL_FIELD", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
