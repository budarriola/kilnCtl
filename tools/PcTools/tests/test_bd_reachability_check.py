"""Unit tests for kilnctrl.bd_reachability_check -- PID_EXPANSION_PLAN.md
sec 3.6b's after-the-fact "did the varied field actually reach the control
law and differ between arms" check, run against two arms' HTTP captures.

Two synthetic arm pairs:
  * a GENUINELY DIFFERING pair (bd_kp_effective differs between arms, as it
    would if e.g. PID_FUZZY were actually engaged in one arm) -> must be
    reported REACHABLE.
  * a BIT-IDENTICAL pair (every bd_* value the same in both arms, as it was
    for the fuzzy-PID inert campaign, commit 8906686) -> must be reported
    INERT.

MANDATORY NEGATIVE TEST: breaks compare_bd_reachability's own equality
check so the bit-identical (inert) pair is wrongly reported as reachable,
confirms the suite fails and names the assertion, then reverts.

Run with: python -m pytest tools/PcTools/tests/test_bd_reachability_check.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import bd_reachability_check as brc  # noqa: E402


def _control_body(zone_bd: dict) -> dict:
    """``zone_bd``: {zone_index: {field: value, ...}}."""
    return {"zones": [dict(bd, zone=z) for z, bd in zone_bd.items()]}


def _write_capture(path: str, control_bodies: list) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        for i, control in enumerate(control_bodies):
            line = {
                "t": 1000.0 + i,
                "exec": {"state": "running", "elapsed_s": i, "dwelling": False,
                          "segment_index": 0, "zones": []},
                "status": {"channels": []},
            }
            if control is not None:
                line["control"] = control
            fh.write(json.dumps(line) + "\n")


class CompareBdReachabilityTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _path(self, name):
        return os.path.join(self.tmpdir, name)

    def test_genuinely_differing_pair_is_reachable(self):
        # Arm A: bd_kp_effective sits at the plain-PID gain (1.0) every
        # sample. Arm B: it's actually being adjusted (PID_FUZZY engaged),
        # so it varies and differs in mean from arm A.
        path_a = self._path("arm_a.jsonl")
        path_b = self._path("arm_b.jsonl")
        _write_capture(path_a, [
            _control_body({0: {"bd_kp_effective": 1.0, "bd_ki_effective": 0.1}})
            for _ in range(5)
        ])
        _write_capture(path_b, [
            _control_body({0: {"bd_kp_effective": v, "bd_ki_effective": 0.1}})
            for v in (1.2, 1.3, 1.1, 1.25, 1.15)
        ])

        verdicts = brc.compare_bd_reachability(
            path_a, path_b, fields=("bd_kp_effective",), zones=[0])
        self.assertEqual(len(verdicts), 1)
        self.assertFalse(verdicts[0].bit_identical)
        self.assertTrue(brc.overall_reachable(verdicts))

    def test_bit_identical_pair_is_inert(self):
        # Both arms produce EXACTLY the same bd_* numbers every sample --
        # the fuzzy-PID inert-campaign signature: the varied preset field
        # (control_mode:2 in both presets) never gated a different code
        # path, so both arms ran the identical branch and produced
        # identical telemetry.
        path_a = self._path("arm_a_inert.jsonl")
        path_b = self._path("arm_b_inert.jsonl")
        same_samples = [
            _control_body({0: {"bd_kp_effective": 1.0, "bd_ki_effective": 0.1,
                                "bd_kd_effective": 0.0}})
            for _ in range(4)
        ]
        _write_capture(path_a, same_samples)
        _write_capture(path_b, same_samples)

        verdicts = brc.compare_bd_reachability(
            path_a, path_b,
            fields=("bd_kp_effective", "bd_ki_effective", "bd_kd_effective"), zones=[0])
        self.assertEqual(len(verdicts), 3)
        self.assertTrue(all(v.bit_identical for v in verdicts))
        self.assertFalse(brc.overall_reachable(verdicts))

        report = brc.format_report(path_a, path_b, verdicts)
        self.assertIn("VERDICT: INERT", report)
        self.assertIn("8906686", report)

    def test_missing_control_data_refuses_rather_than_guessing(self):
        path_a = self._path("no_control_a.jsonl")
        path_b = self._path("no_control_b.jsonl")
        _write_capture(path_a, [None, None])
        _write_capture(path_b, [None, None])
        with self.assertRaises(brc.ReachabilityCheckError):
            brc.compare_bd_reachability(path_a, path_b, fields=("bd_kp_effective",))

    def test_one_arm_missing_control_data_still_refuses(self):
        path_a = self._path("has_control.jsonl")
        path_b = self._path("no_control.jsonl")
        _write_capture(path_a, [
            _control_body({0: {"bd_kp_effective": 1.0}}) for _ in range(3)
        ])
        _write_capture(path_b, [None, None])
        with self.assertRaises(brc.ReachabilityCheckError):
            brc.compare_bd_reachability(path_a, path_b, fields=("bd_kp_effective",), zones=[0])

    def test_cli_exit_codes(self):
        path_a = self._path("cli_a.jsonl")
        path_b = self._path("cli_b.jsonl")
        _write_capture(path_a, [
            _control_body({0: {"bd_kp_effective": 1.0}}) for _ in range(3)
        ])
        _write_capture(path_b, [
            _control_body({0: {"bd_kp_effective": v}}) for v in (2.0, 2.1, 1.9)
        ])
        self.assertEqual(brc.main([path_a, path_b, "--field", "bd_kp_effective", "--zone", "0"]), 0)

        same = [_control_body({0: {"bd_kp_effective": 1.0}}) for _ in range(3)]
        path_c = self._path("cli_c.jsonl")
        path_d = self._path("cli_d.jsonl")
        _write_capture(path_c, same)
        _write_capture(path_d, same)
        self.assertEqual(brc.main([path_c, path_d, "--field", "bd_kp_effective", "--zone", "0"]), 1)


if __name__ == "__main__":
    unittest.main()
