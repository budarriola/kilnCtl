#!/usr/bin/env python3
"""Unit tests for mcp_server_safety.safety_capture_ct_counts() (Opus review
of 51c084f/c49bb0e, finding 8).

The tool previously recorded a sample only when `ct_counts` CHANGED from the
previous poll -- which discards exactly the quiet, unchanged samples a
noise-floor measurement needs (CURRENT_SENSE.md sec 4's "Measured noise
floor" work). If the ADC genuinely isn't moving, "the value didn't change"
IS the noise-floor observation. This suite proves every poll is now
recorded, and that the achieved sample rate reported is real (measured from
actual elapsed time / poll count), not the old value-change count.

Run with: python -m pytest tools/PcTools/tests/test_safety_capture_ct_counts.py -q
"""
from __future__ import annotations

import os
import re
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_safety as mss  # noqa: E402


class FakeClock:
    """Deterministic monotonic clock: each call to sleep() advances the
    clock by exactly the requested duration, with no real wall-clock delay
    -- keeps this test fast and exact regardless of host load."""

    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        if seconds > 0:
            self.now += seconds


class TestSafetyCaptureCtCounts(unittest.TestCase):
    def _run_with_fake_clock_and_statuses(self, statuses, seconds=1.0):
        """statuses: an iterable of GET /api/status dicts, one per poll (the
        capture loop's poll_interval_s is 0.2s, so `seconds` polls will
        consume len(statuses) values -- StopIteration after that raises,
        which would be a test bug, not a real failure mode)."""
        clock = FakeClock()
        it = iter(statuses)

        def fake_get_status(host):
            return next(it)

        with unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"
        ), unittest.mock.patch.object(
            mss.dashboard_http_client, "get_status", side_effect=fake_get_status
        ), unittest.mock.patch.object(
            mss.time, "monotonic", side_effect=clock.monotonic
        ), unittest.mock.patch.object(
            mss.time, "sleep", side_effect=clock.sleep
        ):
            return mss.safety_capture_ct_counts(seconds=seconds)

    def test_unchanged_counts_are_still_recorded_every_poll(self):
        """Repeated, identical ct_counts -- the pre-fix version would have
        kept only the first (a single 'new' sample); the fix must keep
        every poll, since a fixed-cadence capture cares about every poll,
        not just changes. seconds=1.0 at poll_interval_s=0.2 -> 5 polls
        (loop runs while monotonic() < deadline, advancing by exactly 0.2
        each iteration); a generator supplies as many as the loop asks for,
        so an off-by-one in poll count doesn't fail the test on
        StopIteration."""
        same = {"ct_counts": [100, 200, 300]}
        out = self._run_with_fake_clock_and_statuses(
            (same for _ in iter(int, 1)), seconds=1.0
        )
        m_n = re.search(r"^(\d+) samples", out)
        self.assertIsNotNone(m_n, out)
        self.assertGreaterEqual(int(m_n.group(1)), 5, out)
        self.assertNotIn("error", out)

    def test_achieved_rate_reflects_real_elapsed_time_not_change_count(self):
        """With a fixed 0.2s cadence and no changes at all, the achieved Hz
        reported must be close to 1/poll_interval_s (5.0 Hz), not near-zero
        the way a value-change-only count would report for a static signal."""
        same = {"ct_counts": [1, 2, 3]}
        out = self._run_with_fake_clock_and_statuses(
            (same for _ in iter(int, 1)), seconds=2.0
        )
        m_n = re.search(r"^(\d+) samples", out)
        self.assertIsNotNone(m_n, out)
        self.assertGreaterEqual(int(m_n.group(1)), 10, out)
        # Extract "(X.XX Hz achieved" from the summary line.
        m = re.search(r"\(([\d.]+) Hz achieved", out)
        self.assertIsNotNone(m, out)
        hz = float(m.group(1))
        self.assertAlmostEqual(hz, 5.0, delta=0.5, msg=out)

    def test_changing_counts_all_recorded_with_correct_values(self):
        statuses = [
            {"ct_counts": [10, 20, 30]},
            {"ct_counts": [11, 20, 30]},
            {"ct_counts": [11, 20, 30]},  # unchanged from previous -- must still be kept
            {"ct_counts": [12, 21, 31]},
        ]

        def gen():
            yield from statuses
            while True:
                yield statuses[-1]

        out = self._run_with_fake_clock_and_statuses(gen(), seconds=0.8)
        self.assertIn("4 samples", out, out)

    def test_no_valid_frame_still_errors(self):
        """ct_counts absent on every poll: unchanged failure-mode behavior."""
        out = self._run_with_fake_clock_and_statuses(
            ({"uptime_s": 1} for _ in iter(int, 1)), seconds=1.0
        )
        self.assertIn("error", out)
        self.assertIn("ct_counts never present", out)

    # NEGATIVE TEST (negative-test-every-check discipline): simulate the
    # PRE-FIX behavior (dedup on value-change) directly and confirm it would
    # have produced a DIFFERENT (smaller) sample count than the real,
    # fixed function -- proves the "5 samples" assertions above are not
    # vacuously true regardless of the dedup logic.
    def test_negative_old_dedup_behavior_would_have_reported_fewer_samples(self):
        same = {"ct_counts": [100, 200, 300]}
        statuses = [same] * 5

        # Reimplementation of the OLD (buggy) loop body's dedup counting,
        # against the exact same input, to get its sample count.
        last_counts = None
        old_sample_count = 0
        for s in statuses:
            c = tuple(s["ct_counts"])
            if c != last_counts:
                old_sample_count += 1
                last_counts = c
        self.assertEqual(old_sample_count, 1, "sanity: old dedup logic keeps only the first of 5 identical polls")

        out = self._run_with_fake_clock_and_statuses(statuses, seconds=1.0)
        self.assertIn("5 samples", out, out)
        self.assertNotIn("1 samples", out, out)

    # --- Argument validation ------------------------------------------------

    def test_seconds_zero_is_rejected_without_touching_the_wire(self):
        with unittest.mock.patch.object(mss.dashboard_http_client, "get_status") as mock_get:
            out = mss.safety_capture_ct_counts(seconds=0.0)
        self.assertTrue(out.startswith("error"), out)
        self.assertIn("seconds must be > 0", out)
        mock_get.assert_not_called()

    def test_seconds_negative_is_rejected(self):
        with unittest.mock.patch.object(mss.dashboard_http_client, "get_status") as mock_get:
            out = mss.safety_capture_ct_counts(seconds=-5.0)
        self.assertTrue(out.startswith("error"), out)
        mock_get.assert_not_called()

    # --- Error paths ---------------------------------------------------------

    def test_http_failure_surfaces_as_error_string_not_exception(self):
        """GET /api/status raising mid-capture must be reported as an
        `error: ...` string, same convention as every other tool here --
        never let the exception propagate out of the MCP tool call."""
        clock = FakeClock()

        def fake_get_status(host):
            raise OSError("connection refused")

        with unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"
        ), unittest.mock.patch.object(
            mss.dashboard_http_client, "get_status", side_effect=fake_get_status
        ), unittest.mock.patch.object(
            mss.time, "monotonic", side_effect=clock.monotonic
        ), unittest.mock.patch.object(
            mss.time, "sleep", side_effect=clock.sleep
        ):
            out = mss.safety_capture_ct_counts(seconds=1.0)
        self.assertTrue(out.startswith("error"), out)
        self.assertIn("GET /api/status failed", out)

    def test_wrong_length_ct_counts_not_counted_as_valid(self):
        """A `ct_counts` present but not exactly 3 elements (e.g. a
        mid-rollout firmware sending a different channel count) must not be
        treated as a valid sample -- it should behave like `ct_counts`
        absent, not silently unpack a wrong shape."""
        out = self._run_with_fake_clock_and_statuses(
            ({"ct_counts": [1, 2]} for _ in iter(int, 1)), seconds=1.0
        )
        self.assertIn("error", out, out)
        self.assertIn("ct_counts never present", out)

    def test_csv_written_when_out_dir_given(self):
        import tempfile
        same = {"ct_counts": [10, 20, 30]}
        with tempfile.TemporaryDirectory() as tmp:
            # out_dir is not accepted by the helper above, so wire the mocks
            # through directly here, same convention as elsewhere in this file.
            clock = FakeClock()
            it = iter(same for _ in iter(int, 1))

            def fake_get_status(host):
                return next(it)

            with unittest.mock.patch.object(
                mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"
            ), unittest.mock.patch.object(
                mss.dashboard_http_client, "get_status", side_effect=fake_get_status
            ), unittest.mock.patch.object(
                mss.time, "monotonic", side_effect=clock.monotonic
            ), unittest.mock.patch.object(
                mss.time, "sleep", side_effect=clock.sleep
            ):
                out_with_csv = mss.safety_capture_ct_counts(seconds=0.4, out_dir=tmp)
            self.assertIn("csv:", out_with_csv, out_with_csv)
            m = re.search(r"csv: (\S+)", out_with_csv)
            self.assertIsNotNone(m, out_with_csv)
            csv_path = m.group(1)
            self.assertTrue(os.path.isfile(csv_path), csv_path)
            with open(csv_path, "r", encoding="utf-8") as f:
                content = f.read()
            self.assertTrue(content.startswith("ts_ms,ch0,ch1,ch2\n"))
            self.assertIn("10,20,30", content)

    def test_csv_write_failure_reported_as_warning_not_fatal(self):
        """An OSError writing the CSV (e.g. out_dir not creatable) must not
        blow up the whole capture -- the summary is still returned, with a
        warning appended, per the tool's own docstring/handling."""
        same = {"ct_counts": [1, 2, 3]}
        clock = FakeClock()
        it = iter(same for _ in iter(int, 1))

        def fake_get_status(host):
            return next(it)

        with unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"
        ), unittest.mock.patch.object(
            mss.dashboard_http_client, "get_status", side_effect=fake_get_status
        ), unittest.mock.patch.object(
            mss.time, "monotonic", side_effect=clock.monotonic
        ), unittest.mock.patch.object(
            mss.time, "sleep", side_effect=clock.sleep
        ), unittest.mock.patch.object(
            mss.os, "makedirs", side_effect=OSError("permission denied")
        ):
            out = mss.safety_capture_ct_counts(seconds=0.4, out_dir="/no/such/place")
        self.assertNotIn("csv:", out, out)
        self.assertIn("warning", out, out)
        self.assertIn("CSV write failed", out, out)
        # summary stats must still be present -- the failure is non-fatal
        self.assertIn("samples over", out, out)


if __name__ == "__main__":
    unittest.main()
