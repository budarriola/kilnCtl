#!/usr/bin/env python3
"""Pins ``kilnctrl.cone_table`` against ``firmware/KilnFW/App/drivers/
cone_table.c`` (commit d01dfe9). See ``cone_table.py``'s module docstring
for the float32-vs-float64 precision note.

The expected values below are literal constants, independently computed
(``band_bottom = target - (target - lower_cone)/2``, then the Arrhenius
form ``exp(-Ea/(R*T_kelvin))`` normalised between band-bottom and target,
Ea=300000 J/mol, R=8.314 J/(mol*K), T in Kelvin) rather than by calling into
this module -- so a bug shared between the module and its own test cannot
hide behind them. Values were computed by a standalone script reproducing
only the documented formula, not by importing ``cone_table``.
"""
from __future__ import annotations

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import cone_table as ct  # noqa: E402

# Path to the C source this module mirrors, from the repo root.
_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_CONE_TABLE_C_PATH = os.path.join(
    _REPO_ROOT, "firmware", "KilnFW", "App", "drivers", "cone_table.c"
)

# Matches one s_cones[] entry, e.g.  {"022", 586.1f},
_C_ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*(-?\d+(?:\.\d+)?)f\s*\}')


def _parse_c_cone_table(path: str) -> list[tuple[str, float]]:
    """Extracts the ``s_cones[]`` initializer entries out of cone_table.c by
    parsing its source text directly -- this is the enforcement mechanism
    for the "MIRROR DISCIPLINE" this module's docstring and cone_table.py's
    docstring both assert but which, before this test existed, nothing
    actually checked: correcting a value in the C table alone did not fail
    any Python test. Deliberately a plain-text scan (no C compiler
    available here), scoped to the `static const cone_table_entry_t
    s_cones[CONE_TABLE_COUNT] = { ... };` block so it can't accidentally
    match an unrelated brace-and-float pair elsewhere in the file."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    start_marker = "s_cones[CONE_TABLE_COUNT] = {"
    start = text.index(start_marker) + len(start_marker)
    end = text.index("};", start)
    body = text[start:end]

    entries = [(label, float(value)) for label, value in _C_ENTRY_RE.findall(body)]
    if not entries:
        raise AssertionError(f"parsed zero cone entries out of {path} -- parser or marker is broken")
    return entries


class ConeTableCrossLanguagePinTest(unittest.TestCase):
    """Enforces the mirror between cone_table.c's s_cones[] and Python's
    CONE_TABLE: entry count, cone labels (in order), and temperatures. This
    is the test that was claimed (in both files' module docstrings) but
    never existed -- without it, a change to only one side goes undetected
    by the test suite in either language."""

    def test_python_table_matches_c_source_exactly(self):
        c_entries = _parse_c_cone_table(_CONE_TABLE_C_PATH)
        py_entries = list(ct.CONE_TABLE)

        self.assertEqual(
            len(c_entries), len(py_entries),
            f"cone_table.c has {len(c_entries)} entries, cone_table.py has {len(py_entries)} -- mirror broken",
        )
        self.assertEqual(
            [label for label, _ in c_entries], [label for label, _ in py_entries],
            "cone labels (and their order) differ between cone_table.c and cone_table.py",
        )
        for (c_label, c_temp), (py_label, py_temp) in zip(c_entries, py_entries):
            # C stores float32; Python stores float64 -- compare at a
            # tolerance well above float32 rounding noise but far tighter
            # than any real table-entry discrepancy (entries differ by at
            # least several degrees C from their neighbours).
            self.assertAlmostEqual(
                c_temp, py_temp, places=3,
                msg=f"cone '{c_label}'/'{py_label}': C={c_temp} vs Python={py_temp}",
            )


class ConeTableValuesTests(unittest.TestCase):
    def test_table_size_and_endpoints(self):
        self.assertEqual(ct.CONE_TABLE_COUNT, 36)
        self.assertEqual(ct.CONE_TABLE[0], ("022", 586.1))
        self.assertEqual(ct.CONE_TABLE[-1], ("14", 1365.0))

    def test_table_spacing_is_non_uniform(self):
        # Bottom-of-range spacing (022->021) is much tighter than
        # top-of-range spacing (13->14) -- pinning this catches an
        # accidental "smooth the table" edit (cone_table.h forbids it).
        low_gap = ct.CONE_TABLE[1][1] - ct.CONE_TABLE[0][1]
        high_gap = ct.CONE_TABLE[-1][1] - ct.CONE_TABLE[-2][1]
        self.assertAlmostEqual(low_gap, 13.9, places=6)
        self.assertAlmostEqual(high_gap, 33.9, places=6)
        self.assertLess(low_gap, high_gap)

    def test_band_bottom_cone6(self):
        # target = cone 6 (1222.2), lower cone = cone 5 (1186.1).
        self.assertAlmostEqual(ct.band_bottom_c(1222.2), 1204.15, places=6)

    def test_band_bottom_cone021(self):
        # target = cone 021 (600.0), lower cone = cone 022 (586.1).
        self.assertAlmostEqual(ct.band_bottom_c(600.0), 593.05, places=6)

    def test_band_bottom_at_or_below_lowest_cone_raises(self):
        with self.assertRaises(ct.ConeTableError):
            ct.band_bottom_c(586.1)
        with self.assertRaises(ct.ConeTableError):
            ct.band_bottom_c(500.0)

    def test_band_bottom_above_highest_cone_raises(self):
        with self.assertRaises(ct.ConeTableError):
            ct.band_bottom_c(1400.0)

    def test_heat_work_weight_at_target_is_one(self):
        self.assertEqual(ct.heat_work_weight(1222.2, 1222.2), 1.0)
        self.assertEqual(ct.heat_work_weight(1300.0, 1222.2), 1.0)  # above target clamps

    def test_heat_work_weight_at_band_bottom_is_zero(self):
        self.assertEqual(ct.heat_work_weight(1204.15, 1222.2), 0.0)
        self.assertEqual(ct.heat_work_weight(1100.0, 1222.2), 0.0)  # below band bottom clamps

    def test_heat_work_weight_midband_cone6_pinned(self):
        # current = midpoint of [1204.15, 1222.2] = 1213.175.
        w = ct.heat_work_weight(1213.175, 1222.2)
        self.assertAlmostEqual(w, 0.46623880218944525, places=9)

    def test_heat_work_weight_near_target_cone6_pinned(self):
        w = ct.heat_work_weight(1217.2, 1222.2)
        self.assertAlmostEqual(w, 0.6954470338245525, places=9)

    def test_heat_work_weight_near_bottom_cone6_pinned(self):
        w = ct.heat_work_weight(1205.15, 1222.2)
        self.assertAlmostEqual(w, 0.04858494514726569, places=9)

    def test_heat_work_weight_midband_cone021_pinned(self):
        # A second, independent target/cone pair (low-temperature end of
        # the table) so the pin isn't accidentally only exercising cone 6.
        w = ct.heat_work_weight(596.525, 600.0)
        self.assertAlmostEqual(w, 0.4606365210099007, places=9)

    def test_heat_work_weight_monotonic_between_bottom_and_target(self):
        target = 1222.2
        bottom = ct.band_bottom_c(target)
        xs = [bottom + f * (target - bottom) for f in (0.0, 0.1, 0.3, 0.5, 0.7, 0.9, 1.0)]
        weights = [ct.heat_work_weight(x, target) for x in xs]
        for a, b in zip(weights, weights[1:]):
            self.assertLessEqual(a, b + 1e-12)

    def test_heat_work_weight_rejects_non_finite(self):
        with self.assertRaises(ct.ConeTableError):
            ct.heat_work_weight(float("nan"), 1222.2)
        with self.assertRaises(ct.ConeTableError):
            ct.heat_work_weight(1000.0, float("inf"))

    def test_heat_work_weight_rejects_at_or_below_absolute_zero(self):
        with self.assertRaises(ct.ConeTableError):
            ct.heat_work_weight(-273.15, 1222.2)

    def test_cone_for_temp_c(self):
        self.assertEqual(ct.cone_for_temp_c(586.1), 0)
        self.assertEqual(ct.cone_for_temp_c(1365.0), 35)
        self.assertEqual(ct.cone_for_temp_c(2000.0), 35)  # above highest clamps, not an error
        with self.assertRaises(ct.ConeTableError):
            ct.cone_for_temp_c(500.0)  # below lowest


if __name__ == "__main__":
    unittest.main()
