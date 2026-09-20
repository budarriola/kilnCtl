"""Tests for kilnctrl.pico_image_freshness -- the parser and checker behind
firmware/KilnFW/App/test/check_embedded_pico_image_fresh.ps1
(docs/PICO_AUTO_UPDATE_PLAN.md, 2026-09-20 embed-at-boot pass).

Includes a drift guard (test_struct_layout_matches_header) mirroring
test_autotune_rules_drift_guard.py's technique: regex the real C header
directly rather than trusting this module's hand-copied constants to have
stayed in sync, and a negative test (test_stale_record_fails) proving the
freshness check actually FAILS on a planted stale commit rather than merely
not crashing.
"""
from __future__ import annotations

import re
import struct
from pathlib import Path

import pytest

from kilnctrl import pico_image_freshness as fresh

_REPO_ROOT = Path(__file__).resolve().parents[3]
_IDENTITY_HEADER = (
    _REPO_ROOT / "firmware" / "CommonFW" / "include" / "kilnlink" / "saftyfw_image_identity.h"
)


def _make_record(commit: str, dirty: bool = False, config_format_version: int = 3,
                  record_version: int = fresh.RECORD_VERSION,
                  magic_end: int = fresh.MAGIC_END) -> bytes:
    commit_bytes = commit.encode("ascii")
    assert len(commit_bytes) <= fresh.COMMIT_MAX
    padded = commit_bytes + b"\x00" * (fresh.COMMIT_MAX - len(commit_bytes))
    return struct.pack(
        fresh._STRUCT_FMT,
        fresh.MAGIC0, fresh.MAGIC1, record_version,
        1 if dirty else 0, len(commit_bytes), padded,
        config_format_version, 0, magic_end,
    )


# ---------------------------------------------------------------------------
# Drift guard: this module's hand-copied layout constants vs. the real header
# ---------------------------------------------------------------------------


def test_struct_layout_matches_header():
    text = _IDENTITY_HEADER.read_text(encoding="utf-8")
    text_nc = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text_nc = re.sub(r"//.*", "", text_nc)

    def hexconst(name):
        m = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+)u?", text_nc)
        assert m, f"{name} not found in {_IDENTITY_HEADER}"
        return int(m.group(1), 16)

    def intconst(name):
        m = re.search(rf"#define\s+{name}\s+(\d+)u?", text_nc)
        assert m, f"{name} not found in {_IDENTITY_HEADER}"
        return int(m.group(1))

    assert fresh.MAGIC0 == hexconst("SAFTYFW_IMAGE_IDENTITY_MAGIC0")
    assert fresh.MAGIC1 == hexconst("SAFTYFW_IMAGE_IDENTITY_MAGIC1")
    assert fresh.MAGIC_END == hexconst("SAFTYFW_IMAGE_IDENTITY_MAGIC_END")
    assert fresh.RECORD_VERSION == intconst("SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION")
    assert fresh.COMMIT_MAX == intconst("SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX")
    assert fresh.RECORD_SIZE == intconst("SAFTYFW_IMAGE_IDENTITY_SIZE")

    # The struct member order/types themselves -- if a field is reordered or
    # resized this regex still finds the same names but the byte offsets
    # this module assumes (via _STRUCT_FMT) would silently disagree, so
    # also pin the literal field declaration order.
    struct_block_m = re.search(
        r"typedef struct \{(.*?)\}\s*saftyfw_image_identity_t;", text_nc, re.DOTALL
    )
    assert struct_block_m, "saftyfw_image_identity_t struct body not found"
    body = struct_block_m.group(1)
    field_types = re.findall(r"(uint32_t|uint16_t|uint8_t|char)\s+(\w+)(\[[^\]]*\])?;", body)
    names = [f[1] for f in field_types]
    assert names == [
        "magic0", "magic1", "record_version", "dirty", "commit_len",
        "commit", "config_format_version", "reserved", "magic_end",
    ]


# ---------------------------------------------------------------------------
# find_all_identities / find_one_identity
# ---------------------------------------------------------------------------


def test_find_one_identity_in_padded_buffer():
    rec = _make_record("abc1234", dirty=False, config_format_version=3)
    buf = b"\x00" * 136 + rec + b"\xff" * 200
    ident = fresh.find_one_identity(buf)
    assert ident.commit == "abc1234"
    assert ident.dirty is False
    assert ident.config_format_version == 3
    assert ident.offset == 136  # rounded down to 4-byte alignment of the true start


def test_no_record_raises_not_found():
    with pytest.raises(fresh.IdentityNotFound):
        fresh.find_one_identity(b"\x00" * 1000)


def test_truncated_buffer_raises_not_found():
    rec = _make_record("abc1234")
    with pytest.raises(fresh.IdentityNotFound):
        fresh.find_one_identity(rec[:-1])


def test_bad_magic_end_not_matched():
    rec = _make_record("abc1234", magic_end=0xDEADBEEF)
    assert fresh.find_all_identities(rec) == []


def test_two_distinct_records_is_ambiguous():
    rec_a = _make_record("aaaaaaa")
    rec_b = _make_record("bbbbbbb")
    buf = rec_a + rec_b
    with pytest.raises(fresh.MultipleIdentitiesFound):
        fresh.find_one_identity(buf)


def test_two_identical_records_is_not_ambiguous():
    rec = _make_record("abc1234")
    buf = rec + rec
    ident = fresh.find_one_identity(buf)
    assert ident.commit == "abc1234"


# ---------------------------------------------------------------------------
# check_slot_bins_fresh -- the whole check
# ---------------------------------------------------------------------------


def _write_slot(tmp_path, name, commit, dirty=False, extra=b"\xaa" * 64):
    p = tmp_path / name
    p.write_bytes(extra + _make_record(commit, dirty=dirty) + extra)
    return p


def test_skip_when_bins_missing(tmp_path):
    result = fresh.check_slot_bins_fresh(
        tmp_path / "SaftyFW_slotA.bin", tmp_path / "SaftyFW_slotB.bin", None, tmp_path,
    )
    assert result.status == "SKIP"


def test_fail_on_unequal_length(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xaa" * 65)
    result = fresh.check_slot_bins_fresh(a, b, None, tmp_path)
    assert result.status == "FAIL"
    assert "length" in result.message


def test_fail_on_byte_identical_slots(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234")
    b = tmp_path / "b.bin"
    b.write_bytes(a.read_bytes())
    result = fresh.check_slot_bins_fresh(a, b, None, tmp_path)
    assert result.status == "FAIL"
    assert "identical" in result.message


def test_fail_on_mismatched_commits_between_slots(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "def5678", extra=b"\xbb" * 64)
    result = fresh.check_slot_bins_fresh(a, b, None, tmp_path)
    assert result.status == "FAIL"
    assert "disagree" in result.message


def test_dirty_flag_warns_not_fails(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234", dirty=True, extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", dirty=True, extra=b"\xbb" * 64)
    result = fresh.check_slot_bins_fresh(a, b, None, tmp_path)
    assert result.status == "PASS"
    assert "DIRTY" in result.message


def test_skip_when_git_unavailable(tmp_path, monkeypatch):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    monkeypatch.setattr(fresh, "git_short_head", lambda repo_root: None)
    result = fresh.check_slot_bins_fresh(a, b, None, tmp_path)
    assert result.status == "SKIP"


def test_stale_record_fails(tmp_path, monkeypatch):
    """Negative test: a record whose commit does NOT match HEAD must FAIL,
    not silently pass. Without this, a stale-but-present record (the exact
    hazard this check exists to catch -- KilnFW embedding an old SaftyFW
    build) would read as healthy."""
    a = _write_slot(tmp_path, "a.bin", "stale01", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "stale01", extra=b"\xbb" * 64)
    monkeypatch.setattr(fresh, "git_short_head", lambda repo_root: "fresh99")
    result = fresh.check_slot_bins_fresh(a, b, None, tmp_path)
    assert result.status == "FAIL"
    assert "stale01" in result.message
    assert "fresh99" in result.message


def test_skip_when_kilnctrl_bin_missing(tmp_path, monkeypatch):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    monkeypatch.setattr(fresh, "git_short_head", lambda repo_root: "abc1234")
    result = fresh.check_slot_bins_fresh(a, b, tmp_path / "KilnCtrl.bin", tmp_path)
    assert result.status == "SKIP"


def test_fail_when_kilnctrl_bin_has_no_record(tmp_path, monkeypatch):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    kiln = tmp_path / "KilnCtrl.bin"
    kiln.write_bytes(b"\x00" * 2000)
    monkeypatch.setattr(fresh, "git_short_head", lambda repo_root: "abc1234")
    result = fresh.check_slot_bins_fresh(a, b, kiln, tmp_path)
    assert result.status == "FAIL"
    assert "no saftyfw_image_identity_t record" in result.message


def test_fail_when_kilnctrl_bin_embeds_stale_identity(tmp_path, monkeypatch):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    kiln = tmp_path / "KilnCtrl.bin"
    # KilnCtrl.bin embeds an OLDER build than the freshly-rebuilt slot bins.
    kiln.write_bytes(b"\x00" * 500 + _make_record("oldbuild") + b"\x00" * 500)
    monkeypatch.setattr(fresh, "git_short_head", lambda repo_root: "abc1234")
    result = fresh.check_slot_bins_fresh(a, b, kiln, tmp_path)
    assert result.status == "FAIL"
    assert "stale" in result.message


def test_pass_when_kilnctrl_bin_embeds_both_slots(tmp_path, monkeypatch):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    kiln = tmp_path / "KilnCtrl.bin"
    kiln.write_bytes(
        b"\x00" * 300 + _make_record("abc1234", dirty=False)
        + b"\x00" * 300 + _make_record("abc1234", dirty=False) + b"\x00" * 300
    )
    monkeypatch.setattr(fresh, "git_short_head", lambda repo_root: "abc1234")
    result = fresh.check_slot_bins_fresh(a, b, kiln, tmp_path)
    assert result.status == "PASS"


def test_git_short_head_real_repo_matches_git_cli():
    """Sanity check against the real repo (not a fixture): git_short_head()
    must return exactly what a plain `git rev-parse --short HEAD` returns,
    since that is the exact invocation gen_build_info.cmake uses to stamp
    SAFTYFW_GIT_COMMIT -- any difference (e.g. a --short=N length override)
    would make every real board read as stale."""
    import subprocess
    expected = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=str(_REPO_ROOT), capture_output=True, text=True, timeout=10,
    ).stdout.strip()
    if not expected:
        pytest.skip("git not available in this environment")
    assert fresh.git_short_head(_REPO_ROOT) == expected
