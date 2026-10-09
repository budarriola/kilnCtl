#!/usr/bin/env python3
"""Unit tests for the 2026-09-24 SK-01/SK-02 fix (cases_fl.py):
``_dead_by_design_names``/``_judge_against_baseline`` must consult
``task_liveness.py``'s tagged classification of
``check_stack_margin_registration.ps1``'s ``$requiredNames`` list before
scoring a live stack-margin reading, so a boot-once task like
``pico_auto_update`` (which self-deletes after boot by design) is
informational, not a hard "task(s) not running" FAIL.

Per feedback_negative_test_every_check: includes a negative case
(an ``always``-tagged dead task must still FAIL) alongside the fix itself.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_fl_liveness.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_fl as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
from kilnctrl.devices_info import FirmwareVersion, StackMarginEntry  # noqa: E402
from kilnctrl.protocol import StackMarginLevel  # noqa: E402

_SCRIPT_TEMPLATE = """
$requiredNames = @(
    "kiln_io_owner",
    "pico_auto_update",  # liveness: boot-once
    "gpio_probe",  # liveness: config
)
"""


def _entry(name, hwm, configured=4096, alive=True, level=StackMarginLevel.OK):
    return StackMarginEntry(
        name=name, configured_stack_bytes=configured, hwm_bytes=hwm, alive=alive, level=level
    )


def _fw():
    return FirmwareVersion(protocol_version=12, dirty=False, commit="abcdef1", built="2026-09-19T00:00:00Z")


class _TempRepoRoot:
    """Writes a fake tools/check_stack_margin_registration.ps1 under a
    temporary repo root, mirroring the one real script's shape closely
    enough for task_liveness.py's parser."""

    def __enter__(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        (root / "tools").mkdir(parents=True, exist_ok=True)
        (root / "tools" / "check_stack_margin_registration.ps1").write_text(_SCRIPT_TEMPLATE, encoding="utf-8")
        (root / "docs" / "stack_margin_baseline").mkdir(parents=True, exist_ok=True)
        return root

    def __exit__(self, *exc):
        self._tmp.cleanup()


class DeadByDesignNamesTest(unittest.TestCase):
    def test_boot_once_dead_task_is_classified_dead_by_design(self):
        entries = [
            _entry("kiln_io_owner", hwm=1000),
            _entry("pico_auto_update", hwm=0, alive=False),
        ]
        with _TempRepoRoot() as root:
            names = C._dead_by_design_names({"repo_root": str(root)}, entries)
        self.assertEqual(names, ("pico_auto_update",))

    def test_always_tagged_dead_task_is_not_classified_dead_by_design(self):
        """Negative case: an untagged (always) name being dead must NOT be
        exempted -- only boot-once/config/on-demand are."""
        entries = [_entry("kiln_io_owner", hwm=0, alive=False)]
        with _TempRepoRoot() as root:
            names = C._dead_by_design_names({"repo_root": str(root)}, entries)
        self.assertEqual(names, ())

    def test_missing_script_degrades_to_no_exemptions(self):
        entries = [_entry("pico_auto_update", hwm=0, alive=False)]
        with tempfile.TemporaryDirectory() as d:
            names = C._dead_by_design_names({"repo_root": d}, entries)
        self.assertEqual(names, ())


class JudgeAgainstBaselineLivenessTest(unittest.TestCase):
    """Exercises _judge_against_baseline end to end (SK-01's actual case
    function path), with the board/HTTP layer mocked out via ctx["srv"]."""

    def _ctx(self, root, entries):
        srv = mock.Mock()
        srv._info.get_stack_margin.return_value = entries
        srv._info.get_fw_version.return_value = _fw()
        return {"srv": srv, "repo_root": str(root)}

    def test_pico_auto_update_dead_no_longer_fails_sk01(self):
        """This is the exact live-bench failure mode (SK-01/SK-02 FAIL
        'task(s) not running: pico_auto_update'): pico_auto_update has
        self-deleted after boot, every other required task is alive and
        within its committed baseline (empty baseline here -> INCONCLUSIVE
        per-task, never FAIL) -- the case must not FAIL over the boot-once
        task."""
        entries = [
            _entry("kiln_io_owner", hwm=1000),
            _entry("pico_auto_update", hwm=0, alive=False),
        ]
        with _TempRepoRoot() as root:
            result = C._case_sk01(self._ctx(root, entries))
        self.assertNotEqual(result.verdict, Verdict.FAIL)
        self.assertIn("pico_auto_update", result.observed.get("dead_by_design", []))

    def test_an_always_tagged_dead_task_still_fails_sk01(self):
        """Negative test: a genuinely-required task being dead (e.g.
        kiln_io_owner failing to start) must still FAIL -- the fix must not
        blanket-exempt every dead task."""
        entries = [_entry("kiln_io_owner", hwm=0, alive=False)]
        with _TempRepoRoot() as root:
            result = C._case_sk01(self._ctx(root, entries))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("kiln_io_owner", result.reason)


if __name__ == "__main__":
    unittest.main()
