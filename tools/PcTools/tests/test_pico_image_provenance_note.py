"""flash_firmware()'s provenance report now names the embedded SaftyFW
(Pico) image identity -- 2026-09-20 owner decision, docs/PICO_AUTO_UPDATE.md.
This tests only the pure helper, `_pico_image_provenance_note`, not a real
flash (no hardware, no OpenOCD)."""
from __future__ import annotations

from kilnctrl import mcp_server_flash as msf
from kilnctrl import pico_image_freshness as fresh


def _make_record(commit: str, dirty: bool = False, cfg_ver: int = 3, link_proto_ver: int = 0) -> bytes:
    import struct
    commit_b = commit.encode("ascii")
    commit_padded = commit_b + b"\x00" * (fresh.COMMIT_MAX - len(commit_b))
    return struct.pack(
        fresh._STRUCT_FMT,
        fresh.MAGIC0, fresh.MAGIC1, fresh.RECORD_VERSION,
        1 if dirty else 0, len(commit_b), commit_padded,
        cfg_ver, link_proto_ver, fresh.MAGIC_END,
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
    assert "link_protocol_version=0" in note


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


def test_records_differing_only_in_link_protocol_version_flagged(tmp_path):
    """Two records sharing commit/dirty/config_format_version but differing
    in link_protocol_version must still be reported as disagreeing -- the
    same field pico_image_freshness.check_slot_bins_fresh() and the firmware
    mirror (idents_agree() in pico_image_embedded.c) both now compare."""
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(_make_record("aaaaaaa", link_proto_ver=1) + b"\x00" * 8
                  + _make_record("aaaaaaa", link_proto_ver=2))
    note = msf._pico_image_provenance_note(str(p))
    assert "disagree" in note


def test_note_finds_record_in_misaligned_embedded_slot(tmp_path):
    slot = b"\xA5" * 3001 + _make_record("feed123") + b"\x5A" * 64
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(b"\x00" * 1001 + slot)  # slot starts at offset 1001 (not 4-aligned)
    note = msf._pico_image_provenance_note(str(p))
    assert "commit=feed123" in note


def test_note_says_slot_images_not_linked(tmp_path):
    (tmp_path / "KilnFW" / "build").mkdir(parents=True)
    (tmp_path / "SaftyFW" / "build").mkdir(parents=True)
    (tmp_path / "SaftyFW" / "build" / "SaftyFW_slotA.bin").write_bytes(bytes(range(256)) * 20)
    app = tmp_path / "KilnFW" / "build" / "KilnCtrl.bin"
    app.write_bytes(b"\x00" * 5000)
    note = msf._pico_image_provenance_note(str(app))
    assert "NOT present in the app binary" in note
    assert "rebuilt since this app was built" in note
    assert "compiled OFF" in note


def test_note_says_slot_embedded_without_record(tmp_path):
    (tmp_path / "KilnFW" / "build").mkdir(parents=True)
    (tmp_path / "SaftyFW" / "build").mkdir(parents=True)
    slot = bytes(range(256)) * 20
    (tmp_path / "SaftyFW" / "build" / "SaftyFW_slotA.bin").write_bytes(slot)
    app = tmp_path / "KilnFW" / "build" / "KilnCtrl.bin"
    app.write_bytes(b"\x00" * 77 + slot)
    note = msf._pico_image_provenance_note(str(app))
    assert "IS embedded at app offset 77" in note


def test_compiled_off_string_is_informational(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(b"\x00" * 50 + msf._PICO_AUTO_UPDATE_OFF_LITERAL + b": x\x00" + b"\x00" * 50)
    note = msf._pico_image_provenance_note(str(p))
    assert "compiled OFF" in note
    assert "PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT=0" in note
    assert "intentionally absent" in note
    assert "no embedded SaftyFW identity record found" not in note


def test_record_wins_over_compiled_off_string(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(msf._PICO_AUTO_UPDATE_OFF_LITERAL + b"\x00" + _make_record("abc1234"))
    note = msf._pico_image_provenance_note(str(p))
    assert "commit=abc1234" in note
    assert "intentionally absent" not in note


def test_neither_record_nor_off_string_warns(tmp_path):
    p = tmp_path / "KilnCtrl.bin"
    p.write_bytes(b"\x00" * 200)
    note = msf._pico_image_provenance_note(str(p))
    assert "no embedded SaftyFW identity record found" in note
    assert "intentionally absent" not in note


def test_missing_sibling_build_dir_is_stated(tmp_path):
    (tmp_path / "KilnFW" / "build").mkdir(parents=True)
    app = tmp_path / "KilnFW" / "build" / "KilnCtrl.bin"
    app.write_bytes(b"\x00" * 200)
    note = msf._pico_image_provenance_note(str(app))
    assert "cross-check skipped" in note
    assert "not found at" in note
    assert "SaftyFW_slotA.bin" in note
    assert "not located" not in note


def test_off_literal_constant_matches_firmware_source():
    import pathlib
    src = pathlib.Path(__file__).resolve().parents[3] / msf._PICO_AUTO_UPDATE_BOOT_SRC
    assert src.is_file(), src
    assert msf._PICO_AUTO_UPDATE_OFF_LITERAL.decode() in src.read_text(encoding="utf-8")
    assert msf._pico_off_literal_from_source(str(src.parent)) == msf._PICO_AUTO_UPDATE_OFF_LITERAL
