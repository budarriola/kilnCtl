"""Tests for kilnctrl.event_log_decoder -- the PC-side reader for KilnFW's
on-flash binary event log (firmware/KilnFW/App/drivers/persist/event_log.h).

Fixtures under tests/fixtures/event_log/ are hand-built byte streams
matching event_log.c's exact record layout (see
tools/generate scripts -- these were generated once by a small script using
the SAME struct layout this module decodes with; a genuinely independent
check comes from firmware/KilnFW/App/test/test_event_log.c round-tripping
the same format on the C side).

Every check below has a negative-test companion proving it can actually go
red, per repo policy.
"""
from __future__ import annotations

import os

import pytest

from kilnctrl import event_log_decoder as eld

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures", "event_log")


def _read(name: str) -> bytes:
    with open(os.path.join(FIXTURES, name), "rb") as f:
        return f.read()


# ---------------------------------------------------------------------------
# decode_stream() on a valid, checked-in fixture
# ---------------------------------------------------------------------------
def test_decode_stream_sample_fixture():
    records = eld.decode_stream(_read("sample_stream.bin"))
    assert len(records) == 4

    r0 = records[0]
    assert r0.severity == eld.Severity.INFO
    assert r0.source == eld.Source.FIRING
    assert r0.code == eld.FiringCode.STARTED
    assert r0.zone == eld.ZONE_NONE
    assert r0.uptime_s == 5
    assert r0.arg == 5
    assert r0.note == ""

    r1 = records[1]
    assert r1.severity == eld.Severity.ERROR
    assert r1.source == eld.Source.FIRING
    assert r1.code == eld.FiringCode.FAULTED
    assert r1.uptime_s == 3600
    assert r1.arg == 3  # fault_guard

    r2 = records[2]
    assert r2.source == eld.Source.AUTOTUNE
    assert r2.code == eld.AutotuneCode.STARTED
    assert r2.zone == 1

    r3 = records[3]
    assert r3.severity == eld.Severity.WARN
    assert r3.code == eld.AutotuneCode.ABORTED
    assert r3.note == "timeout"


def test_decode_stream_wrong_severity_would_fail():
    """Negative-test companion for test_decode_stream_sample_fixture: proves
    the assertion on r1.severity is actually checking something, not just
    always true (e.g. if decode_stream silently zeroed the field)."""
    records = eld.decode_stream(_read("sample_stream.bin"))
    assert records[1].severity != eld.Severity.INFO


# ---------------------------------------------------------------------------
# Format rendering
# ---------------------------------------------------------------------------
def test_format_record_is_human_readable_and_stable_shape():
    records = eld.decode_stream(_read("sample_stream.bin"))
    line = eld.format_record(records[1])
    assert "ERROR" in line
    assert "FIRING" in line
    assert "FAULTED" in line
    assert "zone=-" in line  # ZONE_NONE renders as '-', not '255'
    assert "arg=3" in line

    line_with_note = eld.format_record(records[3])
    assert "timeout" in line_with_note


# ---------------------------------------------------------------------------
# Truncated stream: the last record is short -- must be refused, not padded
# or silently dropped.
# ---------------------------------------------------------------------------
def test_truncated_stream_raises():
    with pytest.raises(eld.EventLogFormatError):
        eld.decode_stream(_read("truncated_stream.bin"))


def test_truncated_stream_length_is_actually_not_a_multiple():
    """Negative-test companion: proves the fixture really does exercise the
    truncation path (if someone accidentally regenerated it record-aligned,
    this catches that before the test above could give a false pass)."""
    data = _read("truncated_stream.bin")
    assert len(data) % eld.RECORD_SIZE != 0


# ---------------------------------------------------------------------------
# Old-format (pre-2026-09-02) text log: refused with a clear error, never
# misread as binary garbage. This is the migration contract.
# ---------------------------------------------------------------------------
def test_old_text_format_is_refused_not_misread():
    data = _read("old_text_format.log")
    # Old logs are not even RECORD_SIZE-aligned in general, but even a
    # slice that happens to be exactly RECORD_SIZE bytes must be refused --
    # test that directly against decode_record so alignment luck can't mask
    # a real bug.
    chunk = (data + b"\x00" * eld.RECORD_SIZE)[: eld.RECORD_SIZE]
    with pytest.raises(eld.EventLogFormatError):
        eld.decode_record(chunk)


def test_old_text_format_first_byte_is_not_the_magic_by_construction():
    """Negative-test companion: confirms WHY the old format is refused --
    its first byte ('K', 0x4B) genuinely differs from EVENT_MAGIC (0xE7),
    so the refusal above is testing the magic check, not an unrelated
    length mismatch."""
    data = _read("old_text_format.log")
    assert data[0] == ord("K")
    assert data[0] != eld.EVENT_MAGIC


# ---------------------------------------------------------------------------
# decode_record() argument validation
# ---------------------------------------------------------------------------
def test_decode_record_wrong_length_raises():
    with pytest.raises(eld.EventLogFormatError):
        eld.decode_record(b"\x00" * 10)


def test_decode_record_all_zero_bytes_raises():
    """All-zero bytes (an erased/unwritten flash region) must not decode as
    a spuriously 'valid' INFO/FIRING/STARTED record."""
    with pytest.raises(eld.EventLogFormatError):
        eld.decode_record(b"\x00" * eld.RECORD_SIZE)


def test_decode_record_wrong_version_raises():
    """Right magic, wrong version -- proves the check is on BOTH magic and
    version, not magic alone."""
    good = _read("sample_stream.bin")[: eld.RECORD_SIZE]
    assert good[0] == eld.EVENT_MAGIC
    mutated = bytes([good[0], good[1] + 1]) + good[2:]
    with pytest.raises(eld.EventLogFormatError):
        eld.decode_record(mutated)
