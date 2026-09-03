"""Tests for kilnctrl.telemetry_capture -- specifically the append-mode
multi-session hazard (finding 5, opus review round 3): TelemetryCapture used
to always open its output file in append mode, so running
`kilnctrl-telemetry capture --out run1.log` twice concatenated two firings
into one file that parse_profile_exec_uart_capture then reads as one
continuous run -- the same bug a58f0dd fixed for coupling_pair_log's and
log_analysis's PollRow sources the same day, left open here.
"""
from __future__ import annotations

import queue

import pytest

from kilnctrl.telemetry_capture import TelemetryCapture


class _FakeLink:
    """Just enough of UartLink's surface for LogClient's constructor: a
    register_task() that returns a Queue, and a matching release_task() for
    LogClient.close()."""

    def register_task(self, task_id):
        return queue.Queue()

    def unregister_task(self, task_id):
        pass


def test_capture_refuses_to_overwrite_existing_file_by_default(tmp_path):
    """A file that already exists at --out must not be silently reopened in
    append mode -- that is exactly the concatenation hazard.

    Proof this can fail: reverted the fix (open mode hardcoded back to "a").
    Captured red: this test's pytest.raises(RuntimeError) failed with
    `DID NOT RAISE <class 'RuntimeError'>` because a second TelemetryCapture
    against the same path succeeded silently instead of refusing.
    """
    out_path = tmp_path / "run1.log"
    out_path.write_text("12:00:00.000 I existing first firing\n", encoding="utf-8")

    with pytest.raises(RuntimeError) as exc_info:
        TelemetryCapture(_FakeLink(), out_path)
    assert "--append" in str(exc_info.value)


def test_capture_append_true_opts_in_explicitly(tmp_path):
    """append=True must let a capture reopen an existing file (the CLI's
    --append flag), and new lines land after the existing content."""
    out_path = tmp_path / "run1.log"
    out_path.write_text("12:00:00.000 I existing first firing\n", encoding="utf-8")

    cap = TelemetryCapture(_FakeLink(), out_path, append=True)
    try:
        cap._fh.write("12:00:05.000 I second firing\n")
        cap._fh.flush()
    finally:
        cap.close()

    text = out_path.read_text(encoding="utf-8")
    assert "existing first firing" in text
    assert "second firing" in text


def test_capture_creates_a_fresh_file_when_none_exists(tmp_path):
    """The common, non-hazardous case: --out points at a path that does not
    exist yet -- must succeed without needing --append."""
    out_path = tmp_path / "brand_new.log"
    cap = TelemetryCapture(_FakeLink(), out_path)
    try:
        assert out_path.exists()
    finally:
        cap.close()
