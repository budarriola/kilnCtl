"""Parses and checks the ``saftyfw_image_identity_t`` record every SaftyFW
slot image (and, once embedded, the KilnFW application ``.bin``) carries.

Backs ``firmware/KilnFW/App/test/check_embedded_pico_image_fresh.ps1``. Kept
as an importable, unit-testable module rather than logic inline in the
PowerShell wrapper (CLAUDE.md's "Notes for AI Assistants" / repo convention:
non-trivial checking logic lives in Python under tools/PcTools, the .ps1 is a
thin driver -- see e.g. stack_budget_lib_arm.py).

Struct layout is read directly out of
``firmware/CommonFW/include/kilnlink/saftyfw_image_identity.h`` -- copied
here, not re-derived from the compiled firmware -- so
``test_pico_image_freshness_drift_guard`` in
tools/PcTools/tests/test_pico_image_freshness.py can catch the header
changing out from under this parser (same "drift guard" pattern as
test_autotune_rules_drift_guard.py and test_kilnlink_capture.py's fixtures).

The record is found by *scanning*, not by a fixed offset -- see the header's
own "NO FIXED OFFSET, DELIBERATELY" comment. This module does the same scan
independently (does not call into any C code) so a corrupted or absent
record in a real .bin is caught exactly the way a real board's boot-time
scanner would see it.

Known limitation (opus review 2026-09-20, D4): the identity record carries
no slot indicator (slot A vs. slot B). ``check_slot_bins_fresh`` compares
the two slot .bin files by commit/dirty/config_format_version only -- it
cannot detect a build that wrote the same slot's image to both output
paths, or a swap of slotA/slotB content between the two files, since a
swapped pair still agrees on every field this module can see. That defect
class needs a slot indicator added to the record itself to catch
mechanically; it is not caught today.

Scope (opus review 2026-09-20, D1): this module only checks the two
SaftyFW slot .bin files against each other and against repo HEAD. It does
NOT compare against the KilnFW application .bin's embedded copy of the
record -- ``check_00_kilnfw_target_build.ps1`` builds in an isolated
checkbuild worktree under C:/wt/, so ``firmware/KilnFW/build/KilnCtrl.bin``
in the main tree is whatever was built there most recently, not necessarily
fresh, and the standing suite would FAIL on an otherwise healthy tree
whenever that in-tree .bin happened to be stale or absent. The
embedded-in-KilnCtrl.bin comparison still exists, but only in the
flash-time path (`_pico_image_provenance_note()` in mcp_server_flash.py),
where it is a best-effort provenance note on an actual flash, not a
standing pass/fail gate.
"""
from __future__ import annotations

import dataclasses
import struct
import subprocess
from pathlib import Path
from typing import Optional

# --- Struct layout, mirroring saftyfw_image_identity.h --------------------
# typedef struct {
#     uint32_t magic0;
#     uint32_t magic1;
#     uint16_t record_version;
#     uint8_t  dirty;
#     uint8_t  commit_len;
#     char     commit[SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX];  // 40
#     uint16_t config_format_version;
#     uint16_t reserved;
#     uint32_t magic_end;
# } saftyfw_image_identity_t;   // SAFTYFW_IMAGE_IDENTITY_SIZE == 60, little-endian, no padding

MAGIC0 = 0x44494653
MAGIC1 = 0xA5C31E7B
MAGIC_END = 0x7BE1C35A
RECORD_VERSION = 1
COMMIT_MAX = 40
RECORD_SIZE = 60

# "<IIHBB40sHHI" = 4+4+2+1+1+40+2+2+4 = 60
_STRUCT_FMT = "<IIHBB%dsHHI" % COMMIT_MAX
assert struct.calcsize(_STRUCT_FMT) == RECORD_SIZE

SCAN_STEP = 4


@dataclasses.dataclass(frozen=True)
class ImageIdentity:
    record_version: int
    dirty: bool
    commit: str
    config_format_version: int
    offset: int  # byte offset within the buffer the record was found at


class IdentityNotFound(ValueError):
    """No valid saftyfw_image_identity_t record found in the buffer."""


class MultipleIdentitiesFound(ValueError):
    """More than one DISTINCT valid record found -- ambiguous which is real."""


def find_all_identities(buf: bytes) -> "list[ImageIdentity]":
    """Scan `buf` 4 bytes at a time for every offset holding a structurally
    valid record (both magics, the trailer, a recognised version, commit_len
    in bounds). Returns them in offset order. Pure, no side effects."""
    found: list[ImageIdentity] = []
    n = len(buf)
    if n < RECORD_SIZE:
        return found
    last_start = n - RECORD_SIZE
    off = 0
    while off <= last_start:
        magic0, magic1 = struct.unpack_from("<II", buf, off)
        if magic0 == MAGIC0 and magic1 == MAGIC1:
            chunk = buf[off:off + RECORD_SIZE]
            (m0, m1, rec_ver, dirty, commit_len, commit_raw,
             cfg_ver, _reserved, magic_end) = struct.unpack(_STRUCT_FMT, chunk)
            if (magic_end == MAGIC_END and rec_ver == RECORD_VERSION
                    and 0 <= commit_len <= COMMIT_MAX):
                commit = commit_raw[:commit_len].decode("ascii", errors="replace")
                found.append(ImageIdentity(
                    record_version=rec_ver,
                    dirty=bool(dirty),
                    commit=commit,
                    config_format_version=cfg_ver,
                    offset=off,
                ))
        off += SCAN_STEP
    return found


def find_one_identity(buf: bytes) -> ImageIdentity:
    """Like find_all_identities but requires EXACTLY one DISTINCT record
    (same fields; the same bytes appearing at more than one 4-byte-aligned
    offset because of scan overlap is not a real ambiguity and is
    collapsed). Raises IdentityNotFound / MultipleIdentitiesFound
    otherwise."""
    found = find_all_identities(buf)
    if not found:
        raise IdentityNotFound("no saftyfw_image_identity_t record found")
    distinct = {(r.record_version, r.dirty, r.commit, r.config_format_version) for r in found}
    if len(distinct) > 1:
        raise MultipleIdentitiesFound(
            "%d distinct identity records found at offsets %s"
            % (len(distinct), [r.offset for r in found])
        )
    return found[0]


def read_file_identity(path: Path) -> ImageIdentity:
    return find_one_identity(path.read_bytes())


def git_short_head(repo_root: Path) -> Optional[str]:
    """Mirrors gen_build_info.cmake's exact invocation: `git rev-parse
    --short HEAD` run with WORKING_DIRECTORY = the repo root (SaftyFW is
    NOT a submodule -- it shares this main repo -- so this is the same repo
    the check itself runs from). Returns None if git is unavailable or the
    call fails, so callers can SKIP rather than crash."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=str(repo_root), capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    commit = out.stdout.strip()
    return commit or None


@dataclasses.dataclass(frozen=True)
class FreshnessResult:
    status: str  # "PASS", "WARN", "FAIL", "SKIP"
    message: str


def check_slot_bins_fresh(slot_a: Path, slot_b: Path, repo_root: Path) -> FreshnessResult:
    """The whole check, as one importable, testable function.

    Scope (D1): compares the two SaftyFW slot .bin files against each other
    and against repo HEAD only. Does NOT look at KilnFW's application .bin
    at all -- that comparison lives only in the flash-time provenance note
    (`_pico_image_provenance_note()` in mcp_server_flash.py); see this
    module's docstring for why it does not belong in this standing check.
    """
    if not slot_a.exists() or not slot_b.exists():
        return FreshnessResult("SKIP", "SaftyFW slot bins not built yet "
                                        f"({slot_a} / {slot_b})")

    a_bytes = slot_a.read_bytes()
    b_bytes = slot_b.read_bytes()

    if len(a_bytes) != len(b_bytes):
        return FreshnessResult(
            "FAIL",
            f"slotA ({len(a_bytes)} bytes) and slotB ({len(b_bytes)} bytes) "
            "are not equal length -- they must be, since only their slot "
            "position differs (docs/PICO_AUTO_UPDATE_PLAN.md).",
        )
    if a_bytes == b_bytes:
        return FreshnessResult(
            "FAIL",
            "slotA and slotB are byte-identical -- they should differ "
            "(position-dependent linking per slot), which means the build "
            "did not actually produce two distinct slot images.",
        )

    try:
        ident_a = find_one_identity(a_bytes)
    except (IdentityNotFound, MultipleIdentitiesFound) as exc:
        return FreshnessResult("FAIL", f"slotA identity record invalid: {exc}")
    try:
        ident_b = find_one_identity(b_bytes)
    except (IdentityNotFound, MultipleIdentitiesFound) as exc:
        return FreshnessResult("FAIL", f"slotB identity record invalid: {exc}")

    if (ident_a.commit != ident_b.commit or ident_a.dirty != ident_b.dirty
            or ident_a.config_format_version != ident_b.config_format_version):
        return FreshnessResult(
            "FAIL",
            f"slotA identity ({ident_a.commit}, dirty={ident_a.dirty}, "
            f"config_format_version={ident_a.config_format_version}) and "
            f"slotB identity ({ident_b.commit}, dirty={ident_b.dirty}, "
            f"config_format_version={ident_b.config_format_version}) "
            "disagree -- both slots must be built from the same commit and "
            "config schema.",
        )

    if ident_a.dirty:
        # Only the HEAD comparison below is skipped for a dirty build (a
        # dirty working tree's commit is not meaningfully comparable to
        # HEAD) -- there is no other remaining step to fall through to
        # since the KilnCtrl.bin embedding comparison (D1) no longer lives
        # in this function.
        return FreshnessResult(
            "WARN",
            f"SaftyFW slot images carry a DIRTY build (commit {ident_a.commit}) "
            "-- WARNING only, not a failure, per the dirty-flag carve-out.",
        )

    head = git_short_head(repo_root)
    if head is None:
        return FreshnessResult("SKIP", "could not resolve `git rev-parse --short HEAD`")

    if ident_a.commit != head:
        return FreshnessResult(
            "FAIL",
            f"embedded SaftyFW identity commit ({ident_a.commit}) does not "
            f"match the repo's current HEAD ({head}) -- rebuild SaftyFW "
            "(check_00_saftyfw_target_build.ps1) before the KilnFW build "
            "embeds it.",
        )

    return FreshnessResult(
        "PASS",
        f"SaftyFW slot images agree on commit {ident_a.commit} "
        f"(dirty={ident_a.dirty}, config_format_version={ident_a.config_format_version}), "
        "matching repo HEAD.",
    )
