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

from kilnctrl import config_presets, safety_cfg_http_client, zones_http_client  # noqa: E402
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

    def test_missing_preset_raises(self):
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets.load_preset_data("does_not_exist")

    def test_path_traversal_name_rejected(self):
        with self.assertRaises(config_presets.ConfigPresetError):
            config_presets.load_preset_data("../secrets")

    def test_list_presets_includes_bench_fixture(self):
        names = [p["name"] for p in config_presets.list_presets()]
        self.assertIn("bench_fixture", names)


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
        directory = self._write({"name": "broken", "zones": [dict(base), dict(base)]})
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
                                         return_value=ok_result) as mock_apply:
            result = config_presets.apply_preset(fake, preset, zones_host="kiln.local")
        mock_apply.assert_called_once()
        called_host = mock_apply.call_args[0][0]
        self.assertEqual(called_host, "kiln.local")
        self.assertEqual(result.zones_result, ok_result)
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
                                         return_value=failed_result):
            result = config_presets.apply_preset(fake, preset, zones_host="kiln.local")
        self.assertFalse(result.all_ok)
        self.assertIn("max_temp_c", result.describe())
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

    def test_ct_map_lives_only_in_the_backup_section(self):
        preset = config_presets.load_preset_data("bench_fixture")
        for ch in range(3):
            self.assertNotIn(f"ct_channel_map[{ch}]", preset["safety"])
            self.assertIn(f"ct_channel_map[{ch}]", preset["safety_ct_channel_map_backup"])

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
