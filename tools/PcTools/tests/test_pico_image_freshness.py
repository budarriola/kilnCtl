"""Tests for kilnctrl.pico_image_freshness -- the parser and checker behind
firmware/KilnFW/App/test/check_embedded_pico_image_fresh.ps1
(docs/PICO_AUTO_UPDATE.md, 2026-09-20 embed-at-boot pass).

Includes a drift guard (test_struct_layout_matches_header) mirroring
test_autotune_rules_drift_guard.py's technique: regex the real C header
directly rather than trusting this module's hand-copied constants to have
stayed in sync, and a negative test (test_stale_record_fails) proving the
freshness check actually FAILS on a planted stale commit rather than merely
not crashing.
"""
from __future__ import annotations

import os
import re
import struct
import sys
from pathlib import Path

import pytest

from kilnctrl import pico_image_freshness as fresh

_REPO_ROOT = Path(__file__).resolve().parents[3]
_IDENTITY_HEADER = (
    _REPO_ROOT / "firmware" / "CommonFW" / "include" / "kilnlink" / "saftyfw_image_identity.h"
)


def _make_record(commit: str, dirty: bool = False, config_format_version: int = 3,
                  record_version: int = fresh.RECORD_VERSION,
                  link_protocol_version: int = 16,
                  magic_end: int = fresh.MAGIC_END) -> bytes:
    commit_bytes = commit.encode("ascii")
    assert len(commit_bytes) <= fresh.COMMIT_MAX
    padded = commit_bytes + b"\x00" * (fresh.COMMIT_MAX - len(commit_bytes))
    return struct.pack(
        fresh._STRUCT_FMT,
        fresh.MAGIC0, fresh.MAGIC1, record_version,
        1 if dirty else 0, len(commit_bytes), padded,
        config_format_version, link_protocol_version, magic_end,
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
        "commit", "config_format_version", "link_protocol_version", "magic_end",
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
        tmp_path / "SaftyFW_slotA.bin", tmp_path / "SaftyFW_slotB.bin", tmp_path,
    )
    assert result.status == "SKIP"


def test_fail_on_unequal_length(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xaa" * 65)
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "FAIL"
    assert "length" in result.message


def test_fail_on_byte_identical_slots(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234")
    b = tmp_path / "b.bin"
    b.write_bytes(a.read_bytes())
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "FAIL"
    assert "identical" in result.message


def test_fail_on_mismatched_commits_between_slots(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "def5678", extra=b"\xbb" * 64)
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "FAIL"
    assert "disagree" in result.message


def test_fail_on_mismatched_config_format_version_between_slots(tmp_path):
    """D5: slot agreement must also cover config_format_version, not just
    commit/dirty -- a swapped-schema pair could otherwise pass."""
    a = tmp_path / "a.bin"
    b = tmp_path / "b.bin"
    a.write_bytes(b"\xaa" * 64 + _make_record("abc1234", config_format_version=2) + b"\xaa" * 64)
    b.write_bytes(b"\xbb" * 64 + _make_record("abc1234", config_format_version=3) + b"\xbb" * 64)
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "FAIL"
    assert "disagree" in result.message


def test_dirty_flag_warns_not_fails(tmp_path):
    a = _write_slot(tmp_path, "a.bin", "abc1234", dirty=True, extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", dirty=True, extra=b"\xbb" * 64)
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "WARN"
    assert "DIRTY" in result.message


def test_skip_when_git_unavailable(tmp_path, monkeypatch):
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    monkeypatch.setattr(fresh, "git_saftyfw_scoped_head", lambda repo_root: None)
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "SKIP"


def test_stale_record_fails(tmp_path, monkeypatch):
    """Negative test: a record whose commit does NOT match the scoped
    SaftyFW/CommonFW commit must FAIL, not silently pass. Without this, a
    stale-but-present record (the exact hazard this check exists to catch --
    SaftyFW slot bins built from an older commit than the current tree)
    would read as healthy."""
    a = _write_slot(tmp_path, "a.bin", "stale01", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "stale01", extra=b"\xbb" * 64)
    monkeypatch.setattr(fresh, "git_saftyfw_scoped_head", lambda repo_root: "fresh99")
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "FAIL"
    assert "stale01" in result.message
    assert "fresh99" in result.message


def test_pass_message_names_both_schema_versions(tmp_path, monkeypatch):
    """The PASS message must list both config_format_version and
    link_protocol_version, matching the FAIL/disagree message's coverage --
    previously the PASS message named only config_format_version."""
    a = _write_slot(tmp_path, "a.bin", "abc1234", extra=b"\xaa" * 64)
    b = _write_slot(tmp_path, "b.bin", "abc1234", extra=b"\xbb" * 64)
    monkeypatch.setattr(fresh, "git_saftyfw_scoped_head", lambda repo_root: "abc1234")
    result = fresh.check_slot_bins_fresh(a, b, tmp_path)
    assert result.status == "PASS"
    assert "config_format_version=3" in result.message
    assert "link_protocol_version=16" in result.message


def test_git_saftyfw_scoped_head_real_repo_matches_git_cli():
    """Sanity check against the real repo (not a fixture):
    git_saftyfw_scoped_head() must return exactly what `git log -1
    --format=%h -- <SCOPED_PATHS>` returns, since that is the exact
    invocation gen_build_info.cmake uses to stamp SAFTYFW_GIT_COMMIT
    (docs/PICO_AUTO_UPDATE.md sec 13) -- any difference would make
    every real board read as stale."""
    import subprocess
    expected = subprocess.run(
        ["git", "log", "-1", "--format=%h", "--", *fresh.SCOPED_PATHS],
        cwd=str(_REPO_ROOT), capture_output=True, text=True, timeout=10,
    ).stdout.strip()
    if not expected:
        pytest.skip("git not available in this environment")
    assert fresh.git_saftyfw_scoped_head(_REPO_ROOT) == expected


# ---------------------------------------------------------------------------
# Drift guard: SCOPED_PATHS vs. gen_build_info.cmake vs. stale_check.py
# ---------------------------------------------------------------------------
#
# Three separately-maintained, differently-typed copies of the same
# five-path list exist (docs/PICO_AUTO_UPDATE.md sec 13, review rounds
# 2-3 -- round 1 shipped only two of the three paths kept in sync, missing
# firmware/hwAbstraction; round 2 added the whole firmware/hwAbstraction tree,
# which over-covered esp/, host/, idf/, test/ and README.md that never reach
# the Pico image; round 3 narrowed that single entry to the three actually-
# reached subdirectories -- firmware/hwAbstraction/pico, .../common,
# .../interface -- for a five-path total):
#   - fresh.SCOPED_PATHS (this module, a Python tuple)
#   - firmware/SaftyFW/tools/gen_build_info.cmake's two `execute_process`
#     pathspecs (CMake command args)
#   - stale_check.py's check_saftyfw_stale() project_dirs (a Python list
#     built from os.path.join calls)
# This test regexes the latter two and fails loud if either has drifted
# from fresh.SCOPED_PATHS, rather than trusting three hand-edited copies to
# stay aligned silently.

_GEN_BUILD_INFO_CMAKE = (
    _REPO_ROOT / "firmware" / "SaftyFW" / "tools" / "gen_build_info.cmake"
)
_STALE_CHECK_PY = _REPO_ROOT / "tools" / "PcTools" / "src" / "kilnctrl" / "stale_check.py"


def test_scoped_paths_match_cmake_and_stale_check():
    expected = set(fresh.SCOPED_PATHS)
    assert expected == {
        "firmware/SaftyFW",
        "firmware/CommonFW",
        "firmware/hwAbstraction/pico",
        "firmware/hwAbstraction/common",
        "firmware/hwAbstraction/interface",
    }, (
        "this assertion is deliberately a literal, not a tautology against "
        "itself -- if SCOPED_PATHS is edited, this line must be edited too, "
        "reviewed, and only then do the cmake/stale_check comparisons below "
        "mean anything"
    )

    cmake_text = _GEN_BUILD_INFO_CMAKE.read_text(encoding="utf-8")
    # Both execute_process(COMMAND ${GIT_EXECUTABLE} log -1 ...) and
    # execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain ...)
    # reference the same five CMake variables in the same order:
    # "${THIS_PROJECT_DIR}" "${_commonfw_dir}" "${_hwabstraction_pico_dir}"
    # "${_hwabstraction_common_dir}" "${_hwabstraction_interface_dir}".
    # THIS_PROJECT_DIR is firmware/SaftyFW itself (passed in via
    # -DTHIS_PROJECT_DIR=..., not a literal path in this file), and the rest
    # are set from PROJECT_ROOT plus a literal repo-relative suffix -- assert
    # on those suffixes plus the use of THIS_PROJECT_DIR, since the literal
    # "firmware/SaftyFW" string itself does not appear in this file.
    assert 'set(_commonfw_dir "${PROJECT_ROOT}/firmware/CommonFW")' in cmake_text, (
        "gen_build_info.cmake's CommonFW path literal has changed or moved -- "
        "update this test and fresh.SCOPED_PATHS together"
    )
    assert 'set(_hwabstraction_pico_dir "${PROJECT_ROOT}/firmware/hwAbstraction/pico")' in cmake_text, (
        "gen_build_info.cmake's hwAbstraction/pico path is missing or has changed -- "
        "this is exactly the review-round-1/3 drift this test exists to catch"
    )
    assert 'set(_hwabstraction_common_dir "${PROJECT_ROOT}/firmware/hwAbstraction/common")' in cmake_text, (
        "gen_build_info.cmake's hwAbstraction/common path is missing or has changed"
    )
    assert 'set(_hwabstraction_interface_dir "${PROJECT_ROOT}/firmware/hwAbstraction/interface")' in cmake_text, (
        "gen_build_info.cmake's hwAbstraction/interface path is missing or has changed"
    )
    log_calls = re.findall(
        r"execute_process\(\s*COMMAND \$\{GIT_EXECUTABLE\} log -1 --format=%h -- ([^\n]+)",
        cmake_text,
    )
    status_calls = re.findall(
        r"execute_process\(\s*COMMAND \$\{GIT_EXECUTABLE\} status --porcelain -- ([^\n]+)",
        cmake_text,
    )
    assert len(log_calls) == 1, "expected exactly one `git log -1` execute_process in gen_build_info.cmake"
    assert len(status_calls) == 1, "expected exactly one `git status --porcelain` execute_process in gen_build_info.cmake"
    expected_args = (
        '"${THIS_PROJECT_DIR}" "${_commonfw_dir}" "${_hwabstraction_pico_dir}" '
        '"${_hwabstraction_common_dir}" "${_hwabstraction_interface_dir}"'
    )
    assert log_calls[0].strip() == expected_args, (
        f"gen_build_info.cmake's `git log -1` pathspec is {log_calls[0].strip()!r}, "
        f"expected {expected_args!r} -- commit-field scoping has drifted from SCOPED_PATHS"
    )
    assert status_calls[0].strip() == expected_args, (
        f"gen_build_info.cmake's `git status --porcelain` pathspec is {status_calls[0].strip()!r}, "
        f"expected {expected_args!r} -- dirty-flag scoping has drifted from the commit field "
        "(the exact review-round-2 defect: dirty was left scoped to THIS_PROJECT_DIR alone)"
    )

    stale_check_text = _STALE_CHECK_PY.read_text(encoding="utf-8")
    project_dirs_match = re.search(
        r"def check_saftyfw_stale.*?project_dirs=\[(.*?)\],",
        stale_check_text, re.DOTALL,
    )
    assert project_dirs_match, "check_saftyfw_stale()'s project_dirs list not found -- has it been renamed/restructured?"
    project_dirs_body = project_dirs_match.group(1)
    for suffixes in (
        ('"CommonFW"',),
        ('"hwAbstraction"', '"pico"'),
        ('"hwAbstraction"', '"common"'),
        ('"hwAbstraction"', '"interface"'),
    ):
        needle = '"firmware", ' + ", ".join(suffixes)
        assert needle in project_dirs_body, (
            f"stale_check.py's check_saftyfw_stale() project_dirs is missing "
            f"{needle} -- it has drifted from fresh.SCOPED_PATHS"
        )


def test_scoped_paths_drift_is_actually_caught(monkeypatch, tmp_path):
    """Negative test for the drift guard above: prove the REGEX-BASED file
    comparison in test_scoped_paths_match_cmake_and_stale_check actually
    catches real drift in gen_build_info.cmake's content, not merely the
    literal-set equality assertion at the top of that test.

    Round-3 finding: the previous version of this test only monkeypatched
    fresh.SCOPED_PATHS to a wrong tuple, which trips the FIRST assertion
    (`assert expected == {...}`) before any file is ever read or regexed --
    so it never actually exercised the cmake-file-comparison logic that is
    the real payload of the drift guard. This version instead writes a
    drifted (stale, round-2-shaped) copy of gen_build_info.cmake's relevant
    content to a temp file and points _GEN_BUILD_INFO_CMAKE at it, so the
    regex comparisons themselves are what fail."""
    drifted_cmake = tmp_path / "gen_build_info.cmake"
    drifted_cmake.write_text(
        'set(_commonfw_dir "${PROJECT_ROOT}/firmware/CommonFW")\n'
        'set(_hwabstraction_dir "${PROJECT_ROOT}/firmware/hwAbstraction")\n'
        "execute_process(\n"
        '    COMMAND ${GIT_EXECUTABLE} log -1 --format=%h -- "${THIS_PROJECT_DIR}" "${_commonfw_dir}" "${_hwabstraction_dir}"\n'
        ")\n"
        "execute_process(\n"
        '    COMMAND ${GIT_EXECUTABLE} status --porcelain -- "${THIS_PROJECT_DIR}" "${_commonfw_dir}" "${_hwabstraction_dir}"\n'
        ")\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(sys.modules[__name__], "_GEN_BUILD_INFO_CMAKE", drifted_cmake)
    with pytest.raises(AssertionError):
        test_scoped_paths_match_cmake_and_stale_check()


# ---------------------------------------------------------------------------
# Embedded in a KilnFW application image (EMBED_FILES): the slot starts at an
# arbitrary byte offset of the app, so the record is only 4-aligned relative
# to the slot. The default 4-byte-step scan from app offset 0 misses it.
# ---------------------------------------------------------------------------


def _fake_slot(commit: str = "abc1234") -> bytes:
    body = bytes((i * 7 + 3) & 0xFF for i in range(4096))  # vector-table-ish filler
    return body + _make_record(commit) + body[:128]


@pytest.mark.parametrize("misalign", [0, 1, 2, 3])
def test_find_embedded_identities_at_any_app_alignment(misalign):
    slot = _fake_slot()
    app = b"\xE9" * (0x20000 + misalign) + slot + b"\xFF" * 4093
    found = fresh.find_embedded_identities(app)
    assert len(found) == 1
    assert found[0].commit == "abc1234"
    assert found[0].offset == 0x20000 + misalign + 4096


def test_default_step_misses_misaligned_embedded_record_but_standalone_ok():
    slot = _fake_slot()
    app = b"\x00" * 0x20001 + slot
    assert fresh.find_all_identities(app) == []          # the old blind spot
    assert len(fresh.find_embedded_identities(app)) == 1
    assert len(fresh.find_all_identities(slot)) == 1      # standalone image unaffected


def test_locate_embedded_image():
    slot = _fake_slot()
    app = b"\x11" * 12345 + slot + b"\x22" * 99
    assert fresh.locate_embedded_image(app, slot) == 12345
    assert fresh.locate_embedded_image(b"\x11" * 20000, slot) == -1
    # same prefix but different tail is not a match
    other = slot[:200] + b"\x00" * (len(slot) - 200)
    assert fresh.locate_embedded_image(app, other) == -1
