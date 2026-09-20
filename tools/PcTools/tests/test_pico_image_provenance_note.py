"""flash_firmware()'s provenance report now names the embedded SaftyFW
(Pico) image identity -- 2026-09-20 owner decision, docs/PICO_AUTO_UPDATE_PLAN.md.
This tests only the pure helper, `_pico_image_provenance_note`, not a real
flash (no hardware, no OpenOCD)."""
from __future__ import annotations

from kilnctrl import mcp_server_flash as msf
from kilnctrl import pico_image_freshness as fresh


def _make_record(commit: str, dirty: bool = False, cfg_ver: int = 3) -> bytes:
    import struct
    commit_b = commit.encode("ascii")
    commit_padded = commit_b + b"\x00" * (fresh.COMMIT_MAX - len(commit_b))
    return struct.pack(
        fresh._STRUCT_FMT,
        fresh.MAGIC0, fresh.MAGIC1, fresh.RECORD_VERSION,
        1 if dirty else 0, len(commit_b), commit_padded,
        cfg_ver, 0, fresh.MAGIC_END,
    )


def test_missing_binary():
    note = msf._pico_image_provenance_note("/does/not/exist.bin")
    assert "no embedded SaftyFW identity" in note
    assert "not found" in note


def test_no_record_found(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(b"\x00" * 200)
    note = msf._pico_image_provenance_note(str(p))
    assert "no embedded SaftyFW identity record found" in note


def test_single_matching_record(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(b"\x00" * 16 + _make_record("abc1234") + b"\x00" * 16)
    note = msf._pico_image_provenance_note(str(p))
    assert "commit=abc1234" in note
    assert "DIRTY" not in note
    assert "config_format_version=3" in note


def test_dirty_record_noted(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(_make_record("deadbee", dirty=True))
    note = msf._pico_image_provenance_note(str(p))
    assert "DIRTY build" in note


def test_two_records_agreeing_is_fine(tmp_path):
    rec = _make_record("aaaaaaa")
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(rec + b"\x00" * 8 + rec)
    note = msf._pico_image_provenance_note(str(p))
    assert "commit=aaaaaaa" in note
    assert "disagree" not in note


def test_disagreeing_records_flagged(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(_make_record("aaaaaaa") + b"\x00" * 8 + _make_record("bbbbbbb"))
    note = msf._pico_image_provenance_note(str(p))
    assert "disagree" in note
