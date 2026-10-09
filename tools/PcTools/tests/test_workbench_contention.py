#!/usr/bin/env python3
"""Tests for mcpkit.workbench's build-contention detection and locking glue.

Two things are covered that test_buildlock.py does not:

1. ``_contention_note`` -- the known-signature detector that turns a
   ninja "failed recompaction" or a corrupted build_info.h into an explicit
   "this looks like contention, not a source bug" note in the tool's output,
   instead of leaving the caller to chase a phantom source defect the way
   the first agent who hit this did.
2. ``_run_locked`` -- that a lock timeout is reported as a clear FAILED
   result (not an unhandled exception reaching the MCP transport, and not a
   silent hang).

Run with: python -m pytest tools/PcTools/tests/test_workbench_contention.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcpkit import workbench  # noqa: E402
from mcpkit.buildlock import BuildLockTimeout  # noqa: E402


class ContentionNoteDetectsKnownSignatures(unittest.TestCase):
    def test_ninja_recompaction_is_flagged(self):
        output = "ninja: error: failed recompaction: Permission denied\n"
        note = workbench._contention_note(output)
        self.assertIsNotNone(note)
        self.assertIn("BUILD-CONTENTION", note)

    def test_corrupted_build_info_header_is_flagged(self):
        output = (
            "In file included from drivers/dashboard_http.c:12:\n"
            "build/esp-idf/drivers/build_info.h:5:1: error: unknown type name 'by'\n"
        )
        note = workbench._contention_note(output)
        self.assertIsNotNone(note)
        self.assertIn("BUILD-CONTENTION", note)

    def test_ordinary_compile_error_is_not_flagged(self):
        """A real source bug must NOT be relabeled as contention -- that
        would send someone chasing a lock issue that isn't there."""
        output = "drivers/pid.c:42:5: error: use of undeclared identifier 'ki_term'\n"
        self.assertIsNone(workbench._contention_note(output))

    def test_summarize_includes_the_note_only_on_failure(self):
        ok = workbench._summarize("t", ["cmd"], 0, "all good\n", 1.0)
        self.assertNotIn("BUILD-CONTENTION", ok)

        failed = workbench._summarize(
            "t", ["cmd"], 1, "ninja: error: failed recompaction: Permission denied\n", 1.0)
        self.assertIn("BUILD-CONTENTION", failed)


class RunLockedReportsTimeoutClearly(unittest.TestCase):
    def test_lock_timeout_becomes_a_clear_failed_result_not_an_exception(self):
        with mock.patch.object(workbench, "build_lock") as fake_lock:
            fake_lock.side_effect = BuildLockTimeout("some-build-dir", 12.3, "4242 100")
            result = workbench._run_locked("mytag", "some-build-dir", ["true"])
        self.assertIn("mytag", result)
        self.assertIn("FAILED", result)
        self.assertIn("lock contention", result)
        self.assertIn("some-build-dir", result)

    def test_lock_acquired_runs_the_command_normally(self):
        # No mocking of build_lock itself here -- a real (uncontended) lock
        # acquire/release around a trivial command, proving _run_locked's
        # happy path still reaches _run and returns its OK summary.
        argv = [sys.executable, "-c", "print('hi')"]
        result = workbench._run_locked("mytag", "test-happy-path-resource", argv, timeout=30)
        self.assertIn("OK", result)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
