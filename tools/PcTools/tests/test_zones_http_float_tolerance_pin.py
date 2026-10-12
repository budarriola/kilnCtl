"""Pins zones_http_client's read-back float tolerance (REVIEW_BENCHFX4 LOW-2)."""
from __future__ import annotations

import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))
from kilnctrl import zones_http_client as zh  # noqa: E402

_GET_C = pathlib.Path(__file__).resolve().parents[3] / "firmware/KilnFW/App/drivers/http/zones_http_get.c"


def _firmware_formats() -> dict:
    out = {}
    for m in re.finditer(r'\\"([a-z0-9_]+)\\":(%\.\d+[fg])', _GET_C.read_text(encoding="utf8")):
        out.setdefault(m.group(1), set()).add(m.group(2))
    return out


class FloatToleranceTest(unittest.TestCase):
    def test_format_table_matches_firmware(self):
        fw = _firmware_formats()
        for field, fmt in zh._FLOAT_FIELD_PRINT_FORMAT.items():
            self.assertIn(field, fw, field)
            self.assertIn(fmt, fw[field], f"{field}: table {fmt}, firmware {fw[field]}")

    def test_every_preset_float_field_is_mapped_or_nonfloat(self):
        nonfloat = {"relay_mask", "thermo_mask", "ct_mask", "control_mode", "zone_type", "failsafe_state",
                    "min_on_s", "min_off_s", "name", "tc_type", "timing_profile"}
        for f in zh._PRESET_ZONE_OVERRIDE_FIELDS - nonfloat:
            self.assertIn(f, zh._FLOAT_FIELD_PRINT_FORMAT, f)

    def test_tolerance_below_one_step(self):
        for field, fmt in zh._FLOAT_FIELD_PRINT_FORMAT.items():
            tol = zh._float_tolerance(10.0, 10.0, field)
            if fmt == "%.9g":
                self.assertLess(tol, 1e-4, field)
            else:
                step = 10.0 ** -int(fmt[2:-1])
                self.assertLess(tol, step, field)
                self.assertAlmostEqual(tol, step / 2, places=9, msg=field)

    def test_one_step_mismatch_fails_half_step_passes(self):
        self.assertFalse(zh._float_close(1.0, 1.001, "hyst_c"))
        self.assertTrue(zh._float_close(1.0, 1.0004, "hyst_c"))
        self.assertFalse(zh._float_close(300.0, 300.1, "guard_off_settle_s"))
        self.assertFalse(zh._float_close(1.0, 1.001, "cal_offset_c"))
        self.assertTrue(zh._float_close(1.0, 1.0 + 1e-7, "cal_offset_c"))

    def test_verify_against_preset_catches_one_step(self):
        after = {"zones": [{"index": 0, "hyst_c": 1.001, "guard_off_settle_s": 5.0}]}
        bad = zh._verify_against_preset(after, {"zones": [{"index": 0, "hyst_c": 1.0}]})
        self.assertEqual(len(bad), 1)
        good = zh._verify_against_preset(after, {"zones": [{"index": 0, "guard_off_settle_s": 5.04}]})
        self.assertEqual(good, [])  # 0.04 < 0.05 half step


if __name__ == "__main__":
    unittest.main()
