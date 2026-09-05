"""Unit tests for kilnctrl.capture_pool_provenance -- the general
analysis-time guard that refuses (or flags) a pool of HTTP-capture files
that disagree on a config gate field's reachability.

THE INCIDENT THIS EXISTS FOR: an ad hoc analysis pooled 28 captures at
control_mode: 2 (fuzzy layer never runs) with 1 capture at control_mode: 3
(fuzzy layer runs) and reported a 37,008-sample conclusion whose real n
(mode-3 samples) was 2178.

MANDATORY NEGATIVE TEST: builds that exact pool shape (28 mode-2 files + 1
mode-3 file, standing in as 3 mode-2 files for test speed since the ratio
is not what's being tested), confirms check_pool_gate_consistency flags it
and assert_pool_gate_consistent raises; then breaks the check by disabling
the mismatch comparison, confirms the same pool goes quiet, then reverts and
re-confirms it is caught; then proves a clean, internally-consistent pool
(all mode 3) passes with zero problems.

Run with: python -m pytest tools/PcTools/tests/test_capture_pool_provenance.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import capture_pool_provenance as cpp  # noqa: E402


def _write_capture(path: str, control_mode: int, n_lines: int = 5, zone: int = 0) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        for i in range(n_lines):
            line = {
                "t": 1000.0 + i,
                "exec": {
                    "state": "running", "elapsed_s": i, "dwelling": False,
                    "segment_index": 0,
                    "zones": [{"zone": zone, "control_mode": control_mode, "actual_c": 20.0 + i}],
                },
                "status": {"channels": []},
            }
            fh.write(json.dumps(line) + "\n")


class TestPoolGateConsistency(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _path(self, name):
        return os.path.join(self.tmpdir, name)

    def test_unknown_gate_name_raises(self):
        p = self._path("a.jsonl")
        _write_capture(p, control_mode=3)
        with self.assertRaises(cpp.PoolProvenanceError):
            cpp.check_pool_gate_consistency([p], "no_such_gate")

    def test_gate_with_no_capture_representation_raises(self):
        p = self._path("a.jsonl")
        _write_capture(p, control_mode=3)
        with self.assertRaises(cpp.PoolProvenanceError):
            cpp.summarize_file_gate(p, cpp.gf.find_gate("ct_installed"))

    def test_inert_pool_mixing_mode2_and_mode3_is_flagged(self):
        """THE ACTUAL HISTORICAL SHAPE: N mode-2 (unreachable) captures pooled
        with a mode-3 (reachable) capture."""
        mode2_paths = []
        for i in range(3):
            p = self._path(f"mode2_{i}.jsonl")
            _write_capture(p, control_mode=2)
            mode2_paths.append(p)
        mode3_path = self._path("mode3.jsonl")
        _write_capture(mode3_path, control_mode=3)

        pool = mode2_paths + [mode3_path]
        problems = cpp.check_pool_gate_consistency(pool, "control_mode")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("zone 0", problems[0])
        self.assertIn("REACHABLE", problems[0])
        self.assertIn("UNREACHABLE", problems[0])

        with self.assertRaises(cpp.PoolProvenanceError):
            cpp.assert_pool_gate_consistent(pool, "control_mode")

    def test_negative_control_check_can_be_made_to_miss(self):
        """MANDATORY NEGATIVE TEST. Prove the check is not vacuous: break the
        mismatch test (pretend every file is reachable, as a bug that only
        ever looked at .reachable_zones and forgot .unreachable_zones might),
        confirm the known-bad pool goes UNDETECTED, then revert and
        reconfirm detection."""
        mode2_path = self._path("mode2.jsonl")
        _write_capture(mode2_path, control_mode=2)
        mode3_path = self._path("mode3.jsonl")
        _write_capture(mode3_path, control_mode=3)
        pool = [mode2_path, mode3_path]

        # Sanity: detected under the real implementation.
        self.assertEqual(len(cpp.check_pool_gate_consistency(pool, "control_mode")), 1)

        original = cpp.summarize_file_gate
        try:
            def _broken_always_reachable(path, gate):
                summary = original(path, gate)
                # Pretend every zone was reachable everywhere -- the bug
                # class this negative test targets.
                all_zones = set(summary.values_seen)
                return cpp.FileGateSummary(
                    path=summary.path, values_seen=summary.values_seen,
                    reachable_zones=all_zones, unreachable_zones=set())
            cpp.summarize_file_gate = _broken_always_reachable
            problems = cpp.check_pool_gate_consistency(pool, "control_mode")
            self.assertEqual(
                problems, [],
                "broken always-reachable summarize_file_gate should have gone silent, but "
                "the check still flagged something -- the negative test is not exercising "
                "the code path it claims to")
        finally:
            cpp.summarize_file_gate = original

        # Revert confirmed: back to detecting it.
        self.assertEqual(len(cpp.check_pool_gate_consistency(pool, "control_mode")), 1)

    def test_uniform_mode3_pool_is_consistent(self):
        """A clean pool -- every file agrees the gate is reachable -- must
        pass with zero problems (the check flags MIXING, not "any
        unreachable file exists")."""
        paths = []
        for i in range(4):
            p = self._path(f"clean_{i}.jsonl")
            _write_capture(p, control_mode=3)
            paths.append(p)
        self.assertEqual(cpp.check_pool_gate_consistency(paths, "control_mode"), [])
        cpp.assert_pool_gate_consistent(paths, "control_mode")  # must not raise
        self.assertEqual(cpp.uniformly_unreachable_zones(paths, "control_mode"), [])

    def test_uniform_mode2_pool_is_consistent_but_reported_inert(self):
        """A pool that is uniformly UNREACHABLE agrees with itself (not
        flagged as a mismatch) but is surfaced separately as inert."""
        paths = []
        for i in range(3):
            p = self._path(f"allmode2_{i}.jsonl")
            _write_capture(p, control_mode=2)
            paths.append(p)
        self.assertEqual(cpp.check_pool_gate_consistency(paths, "control_mode"), [])
        self.assertEqual(cpp.uniformly_unreachable_zones(paths, "control_mode"), [0])

    def test_cli_exit_codes(self):
        mode2_path = self._path("cli_mode2.jsonl")
        _write_capture(mode2_path, control_mode=2)
        mode3_path = self._path("cli_mode3.jsonl")
        _write_capture(mode3_path, control_mode=3)

        rc_bad = cpp.main(["control_mode", mode2_path, mode3_path, "--json"])
        self.assertEqual(rc_bad, 1)

        rc_ok = cpp.main(["control_mode", mode3_path, "--json"])
        self.assertEqual(rc_ok, 0)


if __name__ == "__main__":
    unittest.main()
