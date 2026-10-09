#!/usr/bin/env python3
"""Unit tests for kilnctrl.config_presets -- preset loading/validation and
preset -> UART-CONTROL-write mapping, all against a fake ControlClient and
JSON fixtures on disk. No real link and no live board is used or required.

Run with: python -m pytest tools/PcTools/tests/test_config_presets.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

import unittest.mock

from kilnctrl import config_presets, ramp_assist_http_client, safety_cfg_http_client, zones_http_client  # noqa: E402
from kilnctrl.devices import OkReason  # noqa: E402


class FakeControl:
    """Stands in for ControlClient: records every call, replies ok by
    default, and can be told to refuse a specific zone."""

    def __init__(self, refuse_zone: "int | None" = None, reason: str = "out of range"):
        self.pid_calls: "list[tuple]" = []
        self.model_calls: "list[tuple]" = []
        self._refuse_zone = refuse_zone
        self._reason = reason

    def set_zone_pid(self, zone, kp, ki, kd):
        self.pid_calls.append((zone, kp, ki, kd))
        if zone == self._refuse_zone:
            return OkReason(ok=False, reason=self._reason)
        return OkReason(ok=True)

    def set_zone_model(self, zone, k_dc, tau_s, dead_time_s):
        self.model_calls.append((zone, k_dc, tau_s, dead_time_s))
        return OkReason(ok=True)


class PresetsDirTest(unittest.TestCase):
    def test_resolves_to_real_config_presets_dir(self):
        d = config_presets.presets_dir()
        self.assertTrue(os.path.isdir(d), d)
        self.assertTrue(os.path.isfile(os.path.join(d, "bench_fixture.json")))


class LoadPresetDataTest(unittest.TestCase):
    def test_bench_fixture_loads_and_validates(self):
        preset = config_presets.load_preset_data("bench_fixture")
        self.assertEqual(preset["name"], "bench_fixture")
        self.assertEqual(len(preset["zones"]), 3)
        for zone in preset["zones"]:
            self.assertEqual(zone["max_temp_c"], 80.0)

    def test_bench_fixture_carries_the_measured_dead_element_rate(self):
        """thermal_guard.c guard 1's minimum rise rate, per zone.

        Two earlier values here were measuring a defect rather than a jig.
        5.0 C/min (a real kiln's figure) killed every bench firing at t=62s.
        It was replaced by 0.2, which the preset's own comment then had to
        record as ASPIRATIONAL: with guard 1 held open the rig had managed
        0.017 C/min over 39.8 minutes, and the conclusion drawn was that the
        heat path delivered about nothing.

        The heat path was fine. profile_executor.c never asked the safety
        processor to permit heating, so K4 was open for all 39.8 of those
        minutes (heat_enable.h, 2026-08-29). With that fixed, the same rig on
        the same profile went 32.1 -> 47.5 C, rising 3.8 C/min at full duty
        and 2.8 C/min over the slowest sampled stretch.

        0.5 C/min is set from those measurements: ~6x below the slowest
        measured full-duty rise, ~30x above what a non-heating path produces.
        It must be present on EVERY zone (a zone left out silently keeps
        whatever the board had) and must stay above 0 -- 0 does not mean
        "off", it means "substitute the firmware default", which is a
        different fact from an explicitly commissioned value even when the
        two numbers coincide.
        """
        preset = config_presets.load_preset_data("bench_fixture")
        for zone in preset["zones"]:
            self.assertEqual(zone["sanity_rate_c_per_min"], 0.5,
                             f"zone {zone['index']} must carry the measured 0.5 C/min")
            self.assertGreater(zone["sanity_rate_c_per_min"], 0.0)

    def test_sanity_rate_is_a_postable_zone_field(self):
        """...and the preset applier can actually write it: a preset field
        with no entry in zones_http_client's form-key map is REFUSED by
        build_post_body(), not silently dropped, so this is the check that
        the preset value reaches the board at all."""
        self.assertIn("sanity_rate_c_per_min", zones_http_client._ZONE_FIELD_FORM_KEY)
        body = zones_http_client.build_post_body(
            {"thermo_count": 3, "relay_count": 3,
             "timing_profiles": [{"index": 0, "name": "Default"}],
             "zones": [{"index": 0, "sanity_rate_c_per_min": 5.0}]},
            {"zones": [{"index": 0, "sanity_rate_c_per_min": 0.2}]})
        self.assertIn("z0_sanity=0.2", body)

    def test_heater_window_is_at_least_3x_the_min_on_time(self):
        """The window/min-on RELATIONSHIP, on every zone.

        Both numbers can be individually sensible and jointly useless, which
        is exactly what happened here: zone 0 carried a 2000 ms window
        against the 10 s minimum on-time, so every on-time the PID could
        compute (0.4 * 2000 = 800 ms) fell under the minimum and
        heater_output_duty() rendered it as fully OFF. The zone reported
        itself as a PID zone and was physically a bang-bang one; an autotune
        step at duty 0.4 commanded heat for 40 minutes without closing the
        relay once.

        The firmware refuses window < 3x the effective minimum on-time
        (zones_http.h, ZONE_HEATER_WINDOW_MIN_MULTIPLE). This asserts the
        preset satisfies the same rule -- a preset that did not would simply
        be rejected by the board on apply, which is a worse way to find out.
        """
        preset = config_presets.load_preset_data("bench_fixture")
        for zone in preset["zones"]:
            window = zone["heater_window_ms"]
            min_on = zone["heater_min_on_ms"]
            effective_min_on = max(min_on, 10000.0)  # HEATER_MIN_ON_MS_FLOOR
            self.assertGreaterEqual(
                window, 3.0 * effective_min_on,
                f"zone {zone['index']}: a {window} ms window against a {effective_min_on} ms "
                f"minimum on-time cannot render a fractional duty")
            self.assertGreaterEqual(min_on, 10000.0,
                                    f"zone {zone['index']}: min-on is a 10 s hardware floor")

    def test_heater_timing_fields_are_postable(self):
        """...and both halves of the pair can actually be written. A preset
        field with no entry in zones_http_client's form-key map is REFUSED by
        build_post_body(); a field not in _PRESET_ZONE_OVERRIDE_FIELDS is
        silently ignored in favour of whatever the board already had, which
        for THIS pair would mean the 2000 ms window survives the apply."""
        self.assertIn("heater_window_ms", zones_http_client._ZONE_FIELD_FORM_KEY)
        self.assertIn("heater_window_ms", zones_http_client._PRESET_ZONE_OVERRIDE_FIELDS)
        body = zones_http_client.build_post_body(
            {"thermo_count": 3, "relay_count": 3,
             "timing_profiles": [{"index": 0, "name": "Default"}],
             "zones": [{"index": 0, "heater_window_ms": 2000.0}]},
            {"zones": [{"index": 0, "heater_window_ms": 60000.0}]})
        self.assertIn("z0_window=60000", body)
        self.assertNotIn("z0_window=2000", body)

    def test_missing_preset_raises(self):
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets.load_preset_data("does_not_exist")

    def test_path_traversal_name_rejected(self):
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets.load_preset_data("../secrets")

    def test_list_presets_includes_bench_fixture(self):
        names = [p["name"] for p in config_presets.list_presets()]
        self.assertIn("bench_fixture", names)

    def test_coupling_matrix_preset_loads_and_validates(self):
        """The real, loadable preset carrying the adopted coupling matrix --
        see this module's docstring on the two misfiled backup files
        (tuned_baseline_20260831.json, pid_validation_backup.json) that are
        NOT presets and that this preset exists alongside, not instead of."""
        preset = config_presets.load_preset_data("coupling_matrix_20260831")
        self.assertEqual(preset["name"], "coupling_matrix_20260831")
        self.assertEqual(len(preset["zones"]), 3)
        by_index = {z["index"]: z for z in preset["zones"]}
        self.assertEqual(by_index[0]["coupling_coeff"], [0.0, 27.32, 21.72])
        self.assertEqual(by_index[1]["coupling_coeff"], [14.30, 0.0, 22.15])
        self.assertEqual(by_index[2]["coupling_coeff"], [8.33, 12.42, 0.0])

    def test_list_presets_includes_coupling_matrix(self):
        names = [p["name"] for p in config_presets.list_presets()]
        self.assertIn("coupling_matrix_20260831", names)

    def test_pre_adoption_coupling_matrix_preset_loads_and_validates(self):
        """The OLD (pre-adoption) coupling matrix, packaged the same way as
        coupling_matrix_20260831.json, so it can be reapplied for a matched
        A/B against the new matrix. Values are the '-' side of commit
        78f2134's diff to tuned_baseline_20260831.json -- what was actually
        live on the bench immediately before that commit overwrote it."""
        preset = config_presets.load_preset_data("coupling_matrix_pre20260902")
        self.assertEqual(preset["name"], "coupling_matrix_pre20260902")
        self.assertEqual(len(preset["zones"]), 3)
        by_index = {z["index"]: z for z in preset["zones"]}
        self.assertEqual(by_index[0]["coupling_coeff"], [0.0, 12.0586, 6.0039])
        self.assertEqual(by_index[1]["coupling_coeff"], [5.7656, 0.0, 6.7734])
        self.assertEqual(by_index[2]["coupling_coeff"], [2.4062, 4.1094, 0.0])

    def test_list_presets_includes_pre_adoption_coupling_matrix(self):
        names = [p["name"] for p in config_presets.list_presets()]
        self.assertIn("coupling_matrix_pre20260902", names)


class ValidationTest(unittest.TestCase):
    """NEGATIVE-TEST: prove the schema check actually rejects a malformed
    preset (rather than silently accepting anything JSON parses)."""

    def _write(self, data) -> str:
        directory = tempfile.mkdtemp()
        path = os.path.join(directory, "broken.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(data, handle)
        return directory

    def _load_from(self, directory, name):
        orig = config_presets.presets_dir
        config_presets.presets_dir = lambda: directory  # type: ignore[assignment]
        try:
            return config_presets.load_preset_data(name)
        finally:
            config_presets.presets_dir = orig  # type: ignore[assignment]

    def test_missing_zones_field_rejected(self):
        directory = self._write({"name": "broken"})
        with self.assertRaises(config_presets.ConfigPresetError) as ctx:
            self._load_from(directory, "broken")
        self.assertIn("zones", str(ctx.exception))

    def test_empty_zones_list_rejected(self):
        directory = self._write({"name": "broken", "zones": []})
        with self.assertRaises(config_presets.ConfigPresetError):
            self._load_from(directory, "broken")

    def test_zone_missing_required_field_rejected(self):
        directory = self._write({
            "name": "broken",
            "ramp_assist_enabled": False,
            "zones": [{"index": 0, "relay_mask": 1}],
        })
        with self.assertRaises(config_presets.ConfigPresetError) as ctx:
            self._load_from(directory, "broken")
        self.assertIn("control_mode", str(ctx.exception))

    def test_duplicate_zone_index_rejected(self):
        base = {
            "index": 0, "relay_mask": 1, "control_mode": 0, "cal_offset_c": 0.0,
            "pid_kp": 0.0, "pid_ki": 0.0, "pid_kd": 0.0, "max_ramp_c_per_hr": 0.0,
            "max_temp_c": 80.0, "min_temp_c": 0.0,
        }
        directory = self._write({"name": "broken", "ramp_assist_enabled": False,
                                 "zones": [dict(base), dict(base)]})
        with self.assertRaises(config_presets.ConfigPresetError) as ctx:
            self._load_from(directory, "broken")
        self.assertIn("duplicate", str(ctx.exception).lower())

    def test_top_level_not_object_rejected(self):
        directory = self._write([1, 2, 3])
        with self.assertRaises(config_presets.ConfigPresetError):
            self._load_from(directory, "broken")


class ApplyPresetTest(unittest.TestCase):
    def test_applies_pid_for_every_zone(self):
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        result = config_presets.apply_preset(fake, preset)
        self.assertEqual(len(fake.pid_calls), 3)
        self.assertTrue(result.all_ok)
        # zone 0's real gains from the live-board capture, not placeholders
        zone0_call = [c for c in fake.pid_calls if c[0] == 0][0]
        self.assertEqual(zone0_call[1:], (2.0, 0.1, 1.0))

    def test_reports_not_written_fields(self):
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        result = config_presets.apply_preset(fake, preset)
        self.assertIn("max_temp_c", result.not_written)
        self.assertIn("relay_mask", result.not_written)
        self.assertIn("control_mode", result.not_written)

    def test_model_not_sent_when_preset_carries_no_model_fields(self):
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        config_presets.apply_preset(fake, preset)
        self.assertEqual(fake.model_calls, [])

    def test_model_sent_when_preset_carries_model_fields(self):
        preset = {
            "name": "with_model",
            "zones": [{
                "index": 0, "relay_mask": 1, "control_mode": 2, "cal_offset_c": 0.0,
                "pid_kp": 1.0, "pid_ki": 0.0, "pid_kd": 0.0, "max_ramp_c_per_hr": 100.0,
                "max_temp_c": 80.0, "min_temp_c": 0.0,
                "k_dc": 50.0, "tau_s": 120.0, "dead_time_s": 5.0,
            }],
        }
        fake = FakeControl()
        config_presets.apply_preset(fake, preset)
        self.assertEqual(fake.model_calls, [(0, 50.0, 120.0, 5.0)])

    def test_refusal_from_firmware_is_reported_not_swallowed(self):
        """NEGATIVE-TEST: a rejected zone must surface as not-ok with its
        reason, not silently read back as success."""
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl(refuse_zone=1, reason="gain out of range")
        result = config_presets.apply_preset(fake, preset)
        self.assertFalse(result.all_ok)
        zone1 = [z for z in result.zones if z.zone == 1][0]
        self.assertFalse(zone1.pid_ok)
        self.assertEqual(zone1.pid_detail, "gain out of range")
        self.assertIn("FAILED", result.describe())
        self.assertIn("gain out of range", result.describe())


class ApplyPresetZonesHostTest(unittest.TestCase):
    """apply_preset(..., zones_host=...) -- the new HTTP write path,
    exercised with zones_http_client.apply_zone_preset() mocked out (its own
    GET/merge/POST logic is unit-tested in test_zones_http_client.py; this
    only checks config_presets.py wires it correctly)."""

    def test_zones_host_omitted_keeps_old_not_written_behavior(self):
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        result = config_presets.apply_preset(fake, preset)
        self.assertIsNone(result.zones_result)
        self.assertIn("max_temp_c", result.not_written)

    def test_zones_host_given_calls_zones_http_client_and_clears_not_written(self):
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        ok_result = zones_http_client.ZonesApplyResult(ok=True, post_response="ok")
        with unittest.mock.patch.object(zones_http_client, "apply_zone_preset",
                                         return_value=ok_result) as mock_apply, \
             unittest.mock.patch.object(ramp_assist_http_client, "set_enabled",
                                         return_value={"ok": True, "enabled": False}) as mock_ra:
            result = config_presets.apply_preset(fake, preset, zones_host="kiln.local")
        mock_apply.assert_called_once()
        called_host = mock_apply.call_args[0][0]
        self.assertEqual(called_host, "kiln.local")
        self.assertEqual(result.zones_result, ok_result)
        # ramp_assist_enabled is pinned over its own endpoint, not through
        # zones_http_client -- confirm it was actually called with the
        # preset's pinned value (bench_fixture.json pins False).
        mock_ra.assert_called_once_with("kiln.local", False)
        self.assertEqual(result.ramp_assist_result, {"ok": True, "enabled": False})
        # Every ZONE field is now written; what remains in not_written is the
        # safety section, which needs its own safety_host (see
        # SafetySectionTest below).
        self.assertEqual([f for f in result.not_written if not f.startswith("safety.")], [])
        self.assertTrue(result.all_ok)

    def test_zones_write_failure_makes_all_ok_false_even_if_pid_writes_succeeded(self):
        """NEGATIVE TEST: a PID write can succeed while the zones HTTP write
        fails its read-back verification -- all_ok must reflect BOTH, not
        just the UART half."""
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        failed_result = zones_http_client.ZonesApplyResult(
            ok=False, mismatches=["zone 0.max_temp_c: expected 80.0, board reports 0.0"],
            post_response="ok")
        with unittest.mock.patch.object(zones_http_client, "apply_zone_preset",
                                         return_value=failed_result), \
             unittest.mock.patch.object(ramp_assist_http_client, "set_enabled",
                                         return_value={"ok": True, "enabled": False}):
            result = config_presets.apply_preset(fake, preset, zones_host="kiln.local")
        self.assertFalse(result.all_ok)
        self.assertIn("max_temp_c", result.describe())
        self.assertIn("FAILED", result.describe())

    def test_ramp_assist_write_failure_makes_all_ok_false(self):
        """NEGATIVE TEST: the zones HTTP write can succeed while pinning
        ramp_assist_enabled fails -- all_ok must reflect that too, not just
        the zones half. This is the exact silent-invalidation hazard the
        field exists to prevent: a preset must not read as fully applied
        when the one flag that decides whether a firing's ramp/dwell shape
        can be trusted did not actually land."""
        preset = config_presets.load_preset_data("bench_fixture")
        fake = FakeControl()
        ok_result = zones_http_client.ZonesApplyResult(ok=True, post_response="ok")
        with unittest.mock.patch.object(zones_http_client, "apply_zone_preset",
                                         return_value=ok_result), \
             unittest.mock.patch.object(ramp_assist_http_client, "set_enabled",
                                         return_value={"ok": False, "error": "ESP_FAIL"}):
            result = config_presets.apply_preset(fake, preset, zones_host="kiln.local")
        self.assertFalse(result.all_ok)
        self.assertIn("ramp_assist_enabled", result.describe())
        self.assertIn("FAILED", result.describe())


class SafetySectionTest(unittest.TestCase):
    """The "safety" preset section -- the SaftyFW commissioning parameters
    config_presets.py's docstring used to call out of scope."""

    def test_bench_fixture_carries_a_safety_section(self):
        preset = config_presets.load_preset_data("bench_fixture")
        safety = preset["safety"]
        # The values this bench can actually vouch for -- see the preset's
        # own _safety_comment for where each one came from.
        self.assertEqual(safety["tc_type"], 3)          # type K, from safety_tc_type
        self.assertEqual(safety["max_rate_c_per_min"], 0.0)  # S8 ships disabled
        self.assertEqual(safety["borrowed_zone_index"], 0)

    def test_bench_preset_declares_no_cts_instead_of_assuming_a_map(self):
        """The bench_fixture preset used to carry an UNVERIFIED identity CT map
        in an opt-in `safety_ct_channel_map_backup` section, because a CT-less
        board could not otherwise be commissioned at all. `ct_installed = 0`
        (param 0x0109) is the honest replacement: it states an absence anyone
        can confirm by looking at the board, instead of a mapping nobody
        measured. The backup section is retired -- and its absence is asserted,
        not merely un-asserted, so it cannot quietly come back."""
        preset = config_presets.load_preset_data("bench_fixture")
        for ch in range(3):
            self.assertNotIn(f"ct_channel_map[{ch}]", preset["safety"])
        self.assertNotIn("safety_ct_channel_map_backup", preset)
        self.assertEqual(preset["safety"]["ct_installed"], 0)

    def test_ct_map_in_the_safety_section_is_rejected(self):
        """NEGATIVE TEST. A ct_channel_map that applied by default would let
        a routine preset load clear calibration_missing on an unmeasured
        map, i.e. declare a board commissioned on a fiction."""
        with self.assertRaises(config_presets.ConfigPresetError) as ctx:
            config_presets._validate_safety_sections(
                "t", {"safety": {"ct_channel_map[0]": 0}})
        self.assertIn("ct_channel_map[0]", str(ctx.exception))

    def test_non_numeric_safety_value_is_rejected(self):
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets._validate_safety_sections("t", {"safety": {"tc_type": "K"}})

    def test_safety_section_must_be_an_object(self):
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets._validate_safety_sections("t", {"safety": [1, 2, 3]})

    def test_safety_host_omitted_reports_the_fields_as_not_written(self):
        preset = config_presets.load_preset_data("bench_fixture")
        result = config_presets.apply_preset(FakeControl(), preset)
        self.assertIsNone(result.safety_result)
        self.assertIn("safety.tc_type", result.not_written)

    def test_safety_host_given_calls_the_safety_client(self):
        preset = config_presets.load_preset_data("bench_fixture")
        ok = safety_cfg_http_client.SafetyApplyResult(
            ok=True, confirmed=["tc_type"], commissioned_after=False,
            still_unset=["ct_channel_map[0]"])
        with unittest.mock.patch.object(safety_cfg_http_client, "apply_safety_preset",
                                         return_value=ok) as mock_apply:
            result = config_presets.apply_preset(
                FakeControl(), preset, safety_host="kiln.local")
        mock_apply.assert_called_once()
        self.assertEqual(mock_apply.call_args[0][0], "kiln.local")
        self.assertIs(mock_apply.call_args.kwargs["use_ct_map_backup"], False)
        self.assertIs(result.safety_result, ok)
        self.assertIn("still UNSET", result.describe())

    def test_ct_map_backup_opt_in_is_forwarded(self):
        preset = config_presets.load_preset_data("bench_fixture")
        ok = safety_cfg_http_client.SafetyApplyResult(ok=True)
        with unittest.mock.patch.object(safety_cfg_http_client, "apply_safety_preset",
                                         return_value=ok) as mock_apply:
            config_presets.apply_preset(FakeControl(), preset, safety_host="kiln.local",
                                         use_ct_map_backup=True)
        self.assertIs(mock_apply.call_args.kwargs["use_ct_map_backup"], True)

    def test_safety_write_failure_makes_all_ok_false(self):
        """NEGATIVE TEST: the PID writes can all succeed while the safety
        commit is refused (relay ARMED, the real live-bench case) -- all_ok
        must reflect that too."""
        preset = config_presets.load_preset_data("bench_fixture")
        failed = safety_cfg_http_client.SafetyApplyResult(
            ok=False, mismatches=["tc_type: reads back UNSET after the commit"],
            post_reason="commit rejected: relay is ARMED")
        with unittest.mock.patch.object(safety_cfg_http_client, "apply_safety_preset",
                                         return_value=failed):
            result = config_presets.apply_preset(FakeControl(), preset, safety_host="kiln.local")
        self.assertFalse(result.all_ok)
        self.assertIn("ARMED", result.describe())


if __name__ == "__main__":
    unittest.main()
