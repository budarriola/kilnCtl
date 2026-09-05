#!/usr/bin/env python3
"""Tests for kilnctrl.fuzzy_band_probe -- the checked-in, tested offline
membership-band probe for the fuzzy-PID layer (pid_fuzzy.c).

Three groups of tests, in order of importance (matching the tool's own
requirement order):

1. THE MODE-2 GUARD (most important -- see fuzzy_band_probe.py's module
   docstring): a capture whose control_mode never reaches 3 must be
   EXCLUDED and LOUDLY FLAGGED, never silently pooled in. Includes a
   negative test proving a capture that DOES reach mode 3 is NOT excluded,
   so the guard is a real discriminator, not a check that always fires.

2. DRIFT GUARD vs the C firmware: pid_fuzzy_adjust() here is asserted
   against the exact same corner cases
   firmware/KilnFW/App/test/test_pid_fuzzy.c checks on the C side. If the
   two ever disagree, this tool is confidently wrong -- see
   test_autotune_rules_drift_guard.py / test_uart_version_independence.py
   for this repo's established drift-check pattern, which this follows
   (transcribe from the authoritative source's *documented* behavior, not
   from whatever fuzzy_band_probe.py's author believed the C did).

3. REFERENCE VALUES: the real mode-3 capture
   (logs/coupling/fuzzy_ab_20260904d_s50_run1.jsonl, 726 rows/zone, 2178
   zone-samples total) reproduces the known-good numbers an earlier ad hoc
   analysis produced and that a live re-fire is not needed to re-derive.

Run with: python -m pytest tools/PcTools/tests/test_fuzzy_band_probe.py -q
"""
from __future__ import annotations

import json
import math
import os
import unittest
from pathlib import Path

from kilnctrl import fuzzy_band_probe as fbp

_REPO_ROOT = Path(__file__).resolve().parents[3]
_REFERENCE_CAPTURE = _REPO_ROOT / "logs" / "coupling" / "fuzzy_ab_20260904d_s50_run1.jsonl"
_REFERENCE_PRESET = (
    _REPO_ROOT / "tools" / "PcTools" / "config_presets" / "fuzzy_ab_strength50_20260903.json"
)
_MODE2_CAPTURE = _REPO_ROOT / "tools" / "PcTools" / "logs" / "coupling" / "p7_fuzzy0_20260903.jsonl"


def _make_capture(path: Path, rows: "list[dict]") -> None:
    with open(path, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")


def _exec_body(zone_modes: "dict[int, int]") -> dict:
    return {
        "dwelling": False,
        "zones": [{"zone": z, "control_mode": m} for z, m in zone_modes.items()],
    }


def _control_body(target_c: float, zone_data: "dict[int, dict]") -> dict:
    zones = []
    for z, data in zone_data.items():
        entry = {"zone": z, "control_mode": data.get("control_mode", 3)}
        entry.update(data)
        zones.append(entry)
    return {"target_c": target_c, "zones": zones}


class ModeTwoGuardTests(unittest.TestCase):
    """Group 1: the single most valuable behavior of this tool."""

    def setUp(self):
        self.tmp_dir = Path(os.environ.get("TMPDIR", ".")) / "fuzzy_band_probe_test_fixtures"
        self.tmp_dir.mkdir(parents=True, exist_ok=True)
        self.base_gains = {0: (0.02, 0.02, 0.02)}
        self.strength = {0: 50}

    def test_mode2_capture_excluded_and_flagged(self):
        """NEGATIVE TEST (the guard proving itself): a capture whose zone 0
        NEVER leaves control_mode 2 must contribute ZERO samples, and the
        exclusion reason must be spelled out in the report, not merely
        absent from the numeric result."""
        path = self.tmp_dir / "mode2_only.jsonl"
        rows = [
            {"t": 1.0, "exec": _exec_body({0: 2})},
            {"t": 2.0, "exec": _exec_body({0: 2})},
            {"t": 3.0, "exec": _exec_body({0: 2})},
        ]
        _make_capture(path, rows)

        report = fbp.probe_capture_files([str(path)], self.base_gains, self.strength, 8.0, 0.25)

        self.assertEqual(report.per_zone, {}, "mode-2-only capture must contribute zero zones")
        self.assertEqual(report.total_usable_samples, 0)
        self.assertTrue(report.exclusions, "must report the exclusion, not go silent")
        joined = " ".join(report.exclusions)
        self.assertIn("control_mode never reached 3", joined)
        self.assertIn("zone 0", joined)

        text = fbp.format_report(report)
        self.assertIn("NO USABLE SAMPLES", text)
        self.assertIn("EXCLUDED", text)

        # CLI contract: exit 1 when nothing usable survived (see main()).
        rc = fbp.main([str(path), "--error-band", "8.0", "--rate-band", "0.25",
                       "--base-gains", str(_REFERENCE_PRESET)])
        self.assertEqual(rc, 1)

    def test_mode3_capture_with_reconstructable_rate_is_not_excluded(self):
        """Proof the guard is a real discriminator, not a check that always
        excludes: the identical fixture shape, but zone 0 runs mode 3 with a
        full "control" body every tick, must survive and produce samples."""
        path = self.tmp_dir / "mode3_ok.jsonl"
        rows = []
        for i in range(5):
            rows.append({
                "t": float(i),
                "exec": _exec_body({0: 3}),
                "control": _control_body(40.0, {0: {
                    "actual_c": 39.5 + 0.1 * i, "pid_d": 0.004, "bd_kd_effective": 0.64,
                }}),
            })
        _make_capture(path, rows)

        report = fbp.probe_capture_files([str(path)], self.base_gains, self.strength, 8.0, 0.25)
        self.assertFalse(report.exclusions, f"a genuine mode-3 capture must not be excluded: {report.exclusions}")
        self.assertIn(0, report.per_zone)
        self.assertEqual(report.per_zone[0].n_samples, 5)
        self.assertEqual(report.total_usable_samples, 5)

    def test_mixed_mode_capture_excludes_only_the_never_mode3_zone(self):
        """A capture with TWO zones, one that reaches mode 3 and one that
        never does, must exclude only the zone that never ran fuzzy --
        not the whole file, and not silently include the inert zone
        either."""
        path = self.tmp_dir / "mixed.jsonl"
        rows = []
        for i in range(4):
            rows.append({
                "t": float(i),
                "exec": _exec_body({0: 3, 1: 2}),
                "control": _control_body(40.0, {
                    0: {"actual_c": 39.5, "pid_d": 0.004, "bd_kd_effective": 0.64},
                }),
            })
        _make_capture(path, rows)
        base_gains = {0: (0.02, 0.02, 0.02), 1: (0.03, 0.03, 0.03)}
        strength = {0: 50, 1: 50}

        report = fbp.probe_capture_files([str(path)], base_gains, strength, 8.0, 0.25)
        self.assertIn(0, report.per_zone)
        self.assertNotIn(1, report.per_zone)
        joined = " ".join(report.exclusions)
        self.assertIn("zone 1", joined)
        self.assertIn("never reached 3", joined)

    def test_bd_kd_effective_zero_is_skipped_not_div_by_zero(self):
        """A sample with bd_kd_effective==0 cannot reconstruct the rate axis
        (division by zero) -- must be silently skipped from usable samples,
        not raise, and not fabricate a rate value."""
        path = self.tmp_dir / "zero_kd.jsonl"
        rows = [{
            "t": 1.0,
            "exec": _exec_body({0: 3}),
            "control": _control_body(40.0, {0: {
                "actual_c": 39.5, "pid_d": 0.0, "bd_kd_effective": 0.0,
            }}),
        }]
        _make_capture(path, rows)
        report = fbp.probe_capture_files([str(path)], self.base_gains, self.strength, 8.0, 0.25)
        self.assertEqual(report.total_usable_samples, 0)
        self.assertTrue(any("0 usable samples" in e for e in report.exclusions))


class FirmwareDriftGuardTests(unittest.TestCase):
    """Group 2: pid_fuzzy_adjust() must match pid_fuzzy.c exactly on every
    corner case firmware/KilnFW/App/test/test_pid_fuzzy.c checks. Values
    transcribed from that C file (2026-09 revision) -- see its own inline
    commentary for the reasoning behind each case."""

    def test_strength_zero_bit_exact_base_gains(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(500.0, 0.3, 20.0, 0.5, 0.012, 0.0034, 0.056, 0)
        self.assertEqual(kp, 0.012)
        self.assertEqual(ki, 0.0034)
        self.assertEqual(kd, 0.056)

    def test_strength_zero_sanitizes_bad_base_gains(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(10.0, 0.1, 20.0, 0.5, math.nan, -1.0, math.inf, 0)
        self.assertEqual(kp, 0.0)
        self.assertEqual(ki, 0.0)
        self.assertEqual(kd, 0.0)

    def test_pos_error_rising_rate_corner(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.01, 0.01, 0.01, 100)
        self.assertGreater(kp, 0.01)
        self.assertLess(ki, 0.01)
        self.assertGreater(kd, 0.01)

    def test_pos_error_falling_rate_corner(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(500.0, -1.0, 20.0, 0.5, 0.01, 0.01, 0.01, 100)
        self.assertLess(kp, 0.01)
        self.assertGreater(ki, 0.01)
        self.assertLess(kd, 0.01)

    def test_neg_error_falling_rate_corner(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(-500.0, -1.0, 20.0, 0.5, 0.01, 0.01, 0.01, 100)
        self.assertGreater(kp, 0.01)
        self.assertLess(ki, 0.01)
        self.assertGreater(kd, 0.01)

    def test_neg_error_rising_rate_corner(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(-500.0, 1.0, 20.0, 0.5, 0.01, 0.01, 0.01, 100)
        self.assertLess(kp, 0.01)
        self.assertGreater(ki, 0.01)
        self.assertLess(kd, 0.01)

    def test_zero_error_falling_rate_cell(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(0.0, -1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertLess(kp, 0.02)
        self.assertLess(ki, 0.02)
        self.assertGreater(kd, 0.02)

    def test_zero_error_steady_rate_cell(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(0.0, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertLess(kp, 0.02)
        self.assertGreater(ki, 0.02)
        self.assertLess(kd, 0.02)

    def test_zero_error_rising_rate_cell(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(0.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertGreater(kp, 0.02)
        self.assertLess(ki, 0.02)
        self.assertGreater(kd, 0.02)

    def test_pos_error_steady_rate_unchanged_ki_kd(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(500.0, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertGreater(kp, 0.02)
        self.assertEqual(ki, 0.02)
        self.assertEqual(kd, 0.02)

    def test_neg_error_steady_rate_unchanged_ki_kd(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(-500.0, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertGreater(kp, 0.02)
        self.assertEqual(ki, 0.02)
        self.assertEqual(kd, 0.02)

    def test_monotonic_in_strength(self):
        kp30, ki30, kd30 = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 30)
        kp80, ki80, kd80 = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 80)
        self.assertGreater(abs(kp80 - 0.02), abs(kp30 - 0.02))
        self.assertGreater(abs(ki80 - 0.02), abs(ki30 - 0.02))
        self.assertGreater(abs(kd80 - 0.02), abs(kd30 - 0.02))

    def test_huge_inputs_stay_finite_nonnegative(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(1.0e9, -1.0e9, 20.0, 0.5, 0.001, 0.001, 0.001, 100)
        for g in (kp, ki, kd):
            self.assertTrue(math.isfinite(g) and g >= 0.0)

    def test_nan_error_holds_base_gains(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(math.nan, 0.0, 20.0, 0.5, 0.02, 0.03, 0.04, 100)
        self.assertEqual((kp, ki, kd), (0.02, 0.03, 0.04))

    def test_inf_rate_holds_base_gains(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(0.0, math.inf, 20.0, 0.5, 0.02, 0.03, 0.04, 100)
        self.assertEqual((kp, ki, kd), (0.02, 0.03, 0.04))

    def test_strength_50_ceiling(self):
        kp, ki, kd = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 50)
        for g in (kp, ki, kd):
            self.assertLessEqual(abs(g - 0.02), 0.02 * 0.25 + 1e-6)
        self.assertGreater(abs(kp - 0.02), 0.02 * 0.24)

    def test_strength_100_ceiling(self):
        kp, _, _ = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertLessEqual(abs(kp - 0.02), 0.02 * 0.5 + 1e-6)
        self.assertGreater(abs(kp - 0.02), 0.02 * 0.49)

    def test_strength_above_100_clamps(self):
        r100 = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        r200 = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.02, 0.02, 0.02, 200)
        self.assertEqual(r100, r200)

    def test_band_rescale_flips_which_side_of_base_kp(self):
        """PID_EXPANSION_PLAN.md sec 3.6g's own discriminator: rescaling the
        error band from 20 to 7 for the SAME 5.0 degC error must flip which
        side of base_kp the result lands on -- can't happen by coincidence
        if the band argument is a no-op."""
        kp20, _, _ = fbp.pid_fuzzy_adjust(5.0, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        kp7, _, _ = fbp.pid_fuzzy_adjust(5.0, 0.0, 7.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertLess(kp20, 0.02 - 1e-6)
        self.assertGreater(kp7, 0.02 + 1e-6)

    def test_boundary_continuity_error_axis(self):
        kp_below, _, _ = fbp.pid_fuzzy_adjust(19.99, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        kp_at, _, _ = fbp.pid_fuzzy_adjust(20.00, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        kp_above, _, _ = fbp.pid_fuzzy_adjust(20.01, 0.0, 20.0, 0.5, 0.02, 0.02, 0.02, 100)
        self.assertLess(abs(kp_at - kp_below), 0.0005)
        self.assertLess(abs(kp_above - kp_at), 0.0005)

    # ---- NEGATIVE TEST: prove the drift guard can actually fail ----

    def test_negative_a_deliberately_wrong_rule_table_fails_the_corner_check(self):
        """Prove test_pos_error_rising_rate_corner et al. are real
        discriminators: monkeypatch RULE_TABLE with an inverted POS/RISING
        cell and confirm the corner assertion would fail against it."""
        original = fbp.RULE_TABLE
        try:
            broken = [list(row) for row in original]
            broken[2] = list(broken[2])
            broken[2][2] = (-1.0, 1.0, -1.0)  # inverted POS/RISING direction
            fbp.RULE_TABLE = tuple(tuple(row) for row in broken)
            kp, ki, kd = fbp.pid_fuzzy_adjust(500.0, 1.0, 20.0, 0.5, 0.01, 0.01, 0.01, 100)
            self.assertFalse(kp > 0.01, "sanity: the broken table should NOT satisfy the "
                                        "original corner assertion, proving that assertion "
                                        "is a real discriminator")
        finally:
            fbp.RULE_TABLE = original


class ReferenceCaptureTests(unittest.TestCase):
    """Group 3: reproduce the known-good values from the one legitimate
    mode-3 capture. Skips (rather than errors) if the referenced files are
    not present in this checkout -- they are data files, not code, and a
    shallow/partial clone should not fail the suite over their absence."""

    def setUp(self):
        if not _REFERENCE_CAPTURE.is_file():
            self.skipTest(f"reference capture not present: {_REFERENCE_CAPTURE}")
        if not _REFERENCE_PRESET.is_file():
            self.skipTest(f"reference preset not present: {_REFERENCE_PRESET}")
        self.base_gains, self.strength = fbp.load_base_gains_from_preset(str(_REFERENCE_PRESET))

    def test_sample_count_is_2178_not_37008(self):
        """The exact regression this tool exists to prevent: pooling in the
        28 inert mode-2 captures inflated n from 2178 to 37008. A single
        legitimate mode-3 capture must report exactly 2178 usable samples
        (726 rows * 3 zones)."""
        report = fbp.probe_capture_files([str(_REFERENCE_CAPTURE)], self.base_gains,
                                         self.strength, 20.0, 0.5)
        self.assertEqual(report.total_usable_samples, 2178)
        self.assertFalse(report.exclusions, f"the legitimate capture must not be excluded: {report.exclusions}")

    def test_current_bands_are_100pct_zero_steady(self):
        report = fbp.probe_capture_files([str(_REFERENCE_CAPTURE)], self.base_gains,
                                         self.strength, 20.0, 0.5)
        for zone, r in report.per_zone.items():
            self.assertEqual(set(r.cell_occupancy), {"ZERO/ZERO"},
                             f"zone {zone}: expected 100% ZERO/STEADY occupancy at the shipped "
                             f"default band, got {r.cell_occupancy}")

    def _overall_deltas(self, error_band_c, rate_band_c_per_s):
        report = fbp.probe_capture_files([str(_REFERENCE_CAPTURE)], self.base_gains,
                                         self.strength, error_band_c, rate_band_c_per_s)
        kp = max(r.kp.max_abs for r in report.per_zone.values())
        ki = max(r.ki.max_abs for r in report.per_zone.values())
        kd = max(r.kd.max_abs for r in report.per_zone.values())
        return kp, ki, kd

    def test_band_8_0_25(self):
        kp, ki, kd = self._overall_deltas(8.0, 0.25)
        self.assertAlmostEqual(kp, 0.187, places=2)
        self.assertAlmostEqual(ki, 0.129, places=2)
        self.assertAlmostEqual(kd, 0.129, places=2)

    def test_band_7_0_22(self):
        kp, ki, kd = self._overall_deltas(7.0, 0.22)
        self.assertAlmostEqual(kp, 0.231, places=2)
        self.assertAlmostEqual(ki, 0.159, places=2)
        self.assertAlmostEqual(kd, 0.159, places=2)

    def test_band_6_0_20(self):
        kp, ki, kd = self._overall_deltas(6.0, 0.20)
        self.assertAlmostEqual(kp, 0.290, places=2)
        self.assertAlmostEqual(ki, 0.186, places=2)
        self.assertAlmostEqual(kd, 0.186, places=2)

    def test_band_5_0_15(self):
        kp, ki, kd = self._overall_deltas(5.0, 0.15)
        self.assertAlmostEqual(kp, 0.373, places=2)
        self.assertAlmostEqual(ki, 0.265, places=2)
        self.assertAlmostEqual(kd, 0.265, places=2)

    def test_cli_exit_code_success_on_legitimate_capture(self):
        rc = fbp.main([str(_REFERENCE_CAPTURE), "--error-band", "8.0", "--rate-band", "0.25",
                       "--base-gains", str(_REFERENCE_PRESET)])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
