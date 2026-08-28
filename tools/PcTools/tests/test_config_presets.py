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

from kilnctrl import config_presets  # noqa: E402
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


if __name__ == "__main__":
    unittest.main()
