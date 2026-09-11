"""elf_archive.py -- archives the exact ELF that was just flashed, keyed so a
board's own self-reported build identity can find it again later, and looks
it up.

Why this exists (2026-09-10): an ESP panic could not be symbolized because no
ELF matching the running firmware (commit 0dddd435) existed anywhere. The
`firmware/KilnFW/build/elf_archive/` directory already existed and IS
populated -- by `archive_elf.cmake`, invoked as a POST_BUILD step on every
`idf.py build` (see that file), keyed by a SHA256 of the linked ELF. That
mechanism works for the ordinary build-in-place workflow. It has one gap:
`flash_firmware(kiln_fw_root=...)` -- the sanctioned "build from a clean git
worktree at HEAD" path, used specifically when the main tree carries another
session's foreign WIP -- builds in a temporary worktree whose own
`build/elf_archive/` never rides along when that worktree is torn down. That
override is exactly how the unmatched 0dddd435 build was produced.

This module closes that gap from the FLASH side rather than the build side:
`flash_firmware()` (and `debug_program(peer="pico")`) call `archive_kiln_elf`/
`archive_safty_elf` after a confirmed-successful flash, copying whatever ELF
was actually just flashed into the CANONICAL archive location (always under
the main tree's firmware/<KilnFW|SaftyFW>/build/elf_archive/, never the
override's own build dir) and recording a manifest entry keyed by the
identity the board can report about itself later:
  - KilnFW: the embedded esp_app_desc build timestamp (the same string
    dashboard_http.c reports as `fw_build`), plus git commit for context.
  - SaftyFW: SAFTYFW_GIT_COMMIT + SAFTYFW_BUILD_DATE + SAFTYFW_BUILD_TIME
    from the build's own saftyfw_build_info.h (SaftyFW has no HTTP API to
    ask, so there is no runtime-reported timestamp to key on independently --
    this is the same identity stale_check.py already trusts).

This also runs on the ordinary (non-override) path, redundantly with
archive_elf.cmake -- harmless (same content hashes to the same key, second
write is a no-op) and means the manifest (which the cmake step does not
maintain) stays populated even for a plain in-place build+flash.

Lookup (`find_kiln_elf_for_build` / `find_safty_elf_for_identity`) is a
manifest read: given the identity string the board reports, look up the
recorded elf_key and confirm the file is still on disk. No match is reported
loudly with the identity that was searched for and how many entries the
manifest holds -- never a silent fallback to `KilnCtrl-latest.elf` or the
newest-by-mtime file, either of which would produce a plausible WRONG
symbolization exactly like the incident this module exists to prevent.

Retention: `prune_archive` caps each archive directory at `MAX_ARCHIVED_ELFS`
entries (default 60), deleting the oldest-by-mtime files first, and never
deletes `<prefix>-latest.elf` or any entry named in the surviving manifest's
most recent `KEEP_RECENT_ENTRIES` (default 10) rows regardless of age -- a
board flashed weeks ago and only diagnosed today should not have already lost
its ELF. Pruning runs after every archive call, so the directory is bounded
on an ongoing basis rather than needing a separate cron/cleanup step.
`firmware/KilnFW/.gitignore` / `firmware/SaftyFW/.gitignore` already exclude
the whole `build/` tree (elf_archive included), confirmed by
`git check-ignore -v`; nothing here needs to touch .gitignore.

2026-09-10, opus review round 3 (four confirmed defects on this exact
symbolization path, fixed together):
  1. Manifest keys are now migrated to normalized form on every read
     (`_load_manifest`), not just normalized on write/lookup -- a
     pre-existing entry stored under its raw (un-normalized) key used to
     become permanently unreachable once only the write/lookup sides were
     normalized. Reproduced live against the real manifest before the fix.
  2. Verified HEAD imports cleanly (`77eed9f2` already restored
     `esp_app_desc.normalize_build_timestamp` as an ancestor of HEAD; no
     further change needed).
  3. `test_elf_archive.py`'s guard-negative-test no longer writes into the
     real canonical archive under any code path -- it proves the same
     refuse/allow branch against a monkeypatched `_canonical_archive_dirs()`
     result instead.
  4. Genuinely different ELF contents can share one fw_build identity (a
     rebuild that doesn't touch the translation unit embedding
     __DATE__/__TIME__) -- confirmed live: 11 such collisions across 60
     real archived ELFs against only 3 manifest entries, leaving 57 files
     simultaneously unreachable and unprotected from `_prune`. `_archive()`
     now records a superseded elf_key (see `SUPERSEDED_NAME`) instead of
     letting it silently fall out of the manifest; `_prune` protects those
     too, and `find_kiln_elf_for_build` discloses them in its success
     message rather than hiding the ambiguity. The real archive's manifest/
     superseded files were reconstructed once, by hand, from each orphaned
     ELF's own embedded esp_app_desc build timestamp, so every pre-existing
     on-disk ELF is now either reachable or deliberately protected.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import time
from dataclasses import dataclass, asdict
from typing import Optional

from . import stale_check
from .esp_app_desc import normalize_build_timestamp

MAX_ARCHIVED_ELFS = 60
KEEP_RECENT_ENTRIES = 10
MANIFEST_NAME = "manifest.json"
# 2026-09-10 (opus review round 3, defect 4): when a new archive call reuses
# an fw_build identity that the manifest already maps to a DIFFERENT elf_key
# (a rebuild that didn't touch the translation unit embedding __DATE__/
# __TIME__, so two genuinely different ELFs report the identical fw_build
# string -- confirmed live: 11 such collisions across the 60 ELFs sitting in
# firmware/KilnFW/build/elf_archive/ against only 3 manifest entries), the
# manifest keeps mapping that identity to the most-recently-archived elf_key
# (matches existing "last write wins" behavior) but the superseded elf_key is
# recorded here instead of being silently dropped -- unreachable by lookup
# AND unprotected from _prune is exactly the state that let 57 genuine ELFs
# sit one write away from deletion.
SUPERSEDED_NAME = "superseded.json"


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/ -> repo root is four levels up."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


def kiln_archive_dir() -> str:
    """Canonical KilnFW ELF archive dir -- always the MAIN tree's, regardless
    of any kiln_fw_root override used to build/flash."""
    return os.path.join(_repo_root(), "firmware", "KilnFW", "build", "elf_archive")


def safty_archive_dir() -> str:
    return os.path.join(_repo_root(), "firmware", "SaftyFW", "build", "elf_archive")


def _canonical_archive_dirs() -> set[str]:
    """The two real archive directories, computed directly from _repo_root()
    rather than through kiln_archive_dir()/safty_archive_dir() -- those two
    functions are exactly what a test is expected to monkeypatch, so a guard
    that read them back would be blind precisely when it needs to fire."""
    root = _repo_root()
    return {
        os.path.normpath(os.path.join(root, "firmware", "KilnFW", "build", "elf_archive")),
        os.path.normpath(os.path.join(root, "firmware", "SaftyFW", "build", "elf_archive")),
    }


def _guard_against_test_write(archive_dir: str) -> None:
    """2026-09-10: a host test (test_flash_board_pinning.py /
    test_flash_firmware_verify.py) drove flash_firmware() end-to-end while
    mocking OpenOCD, stale_check and flash_provenance -- but not elf_archive
    -- so archive_kiln_elf() ran for real against the CANONICAL archive and
    overwrote the manifest entry for a genuine build ("Sep  9 2026 14:18:51")
    with a fabricated commit ("abc1234", a test fixture string). The archive
    exists specifically to let a real panic be symbolized against the right
    ELF; a contaminated entry produces confident, wrong line numbers.

    Rather than relying on every future test author remembering to patch
    this module (the exact thing that failed here), make it structurally
    impossible: refuse loudly, before touching disk, whenever pytest is
    running (PYTEST_CURRENT_TEST is set by pytest for the duration of every
    test) and the archive_dir in play is one of the two real ones. A test
    that correctly monkeypatches kiln_archive_dir()/safty_archive_dir() (as
    test_elf_archive.py does) is unaffected -- its archive_dir is a temp
    path and never matches. KILNCTL_ALLOW_TEST_ARCHIVE_WRITE=1 is the
    explicit, deliberate escape hatch for a test that really means to
    exercise the canonical path (none does today)."""
    if not os.environ.get("PYTEST_CURRENT_TEST"):
        return
    if os.environ.get("KILNCTL_ALLOW_TEST_ARCHIVE_WRITE"):
        return
    if os.path.normpath(archive_dir) in _canonical_archive_dirs():
        raise RuntimeError(
            "elf_archive: refusing to write to the CANONICAL archive "
            f"({archive_dir}) from inside a pytest run (PYTEST_CURRENT_TEST is "
            "set). This directory holds real symbolization data for real "
            "hardware panics. The calling test must monkeypatch "
            "elf_archive.kiln_archive_dir() / elf_archive.safty_archive_dir() "
            "to point at a temp directory (see test_elf_archive.py), or patch "
            "elf_archive.archive_kiln_elf / elf_archive.archive_safty_elf "
            "directly if it does not need real archiving behavior. Set "
            "KILNCTL_ALLOW_TEST_ARCHIVE_WRITE=1 only if a test deliberately "
            "needs to exercise the canonical path."
        )


def _sha256_key(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def _load_manifest(archive_dir: str) -> dict:
    path = os.path.join(archive_dir, MANIFEST_NAME)
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(raw, dict):
        return {}
    # 2026-09-10 (opus review round 3, defect 1): migrate raw (un-normalized)
    # keys transparently on every read. archive_kiln_elf()/find_kiln_elf_for_
    # build() both now key/look up on normalize_build_timestamp(fw_build),
    # but entries written before that fix -- or hand-repaired directly, like
    # the "Sep  9 2026 14:18:51" (double-space) entry this module's own
    # incident produced -- are still stored under their raw key on disk.
    # Without this, normalizing only the lookup side (and not what's already
    # on disk) makes every such pre-existing entry permanently unreachable:
    # find_kiln_elf_for_build('Sep  9 2026 14:18:51') looked up the
    # normalized 'Sep 9 2026 14:18:51' and missed the raw-keyed entry
    # entirely, reproduced live before this fix. Idempotent for
    # already-normalized keys, and harmless for SaftyFW identity keys
    # (commit_date_time, no internal whitespace runs to collapse).
    manifest: dict = {}
    for key, entry in raw.items():
        norm_key = normalize_build_timestamp(key)
        prior = manifest.get(norm_key)
        if prior is not None and prior.get("seq", 0) > entry.get("seq", 0):
            continue  # keep whichever raw/normalized duplicate is more recent
        manifest[norm_key] = entry
    return manifest


def _write_manifest(archive_dir: str, manifest: dict) -> None:
    path = os.path.join(archive_dir, MANIFEST_NAME)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    os.replace(tmp, path)


def _load_superseded(archive_dir: str) -> dict:
    """identity -> list of manifest-shaped entries whose elf_key was bumped
    out of the live manifest slot for that identity by a later archive call
    that reused the same fw_build string for different content (see
    SUPERSEDED_NAME's module-level comment). Kept so _prune can still
    protect those files and find_kiln_elf_for_build can disclose them."""
    path = os.path.join(archive_dir, SUPERSEDED_NAME)
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _write_superseded(archive_dir: str, superseded: dict) -> None:
    path = os.path.join(archive_dir, SUPERSEDED_NAME)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(superseded, f, indent=2, sort_keys=True)
    os.replace(tmp, path)


@dataclass
class ArchiveResult:
    elf_key: str
    archived_path: str
    identity: str  # the lookup key recorded for this entry
    newly_archived: bool  # False if this content was already archived


def _archive(elf_path: str, archive_dir: str, prefix: str, identity: str,
             extra: dict) -> ArchiveResult:
    _guard_against_test_write(archive_dir)
    if not os.path.isfile(elf_path):
        raise FileNotFoundError(f"elf_archive: no ELF at {elf_path} to archive")
    os.makedirs(archive_dir, exist_ok=True)
    elf_key = _sha256_key(elf_path)
    dest = os.path.join(archive_dir, f"{prefix}-{elf_key}.elf")
    newly_archived = not os.path.exists(dest)
    if newly_archived:
        shutil.copyfile(elf_path, dest)
    latest = os.path.join(archive_dir, f"{prefix}-latest.elf")
    shutil.copyfile(elf_path, latest)

    manifest = _load_manifest(archive_dir)
    # 2026-09-10 (opus review round 3, defect 4): if this identity is already
    # mapped to a DIFFERENT elf_key, that older entry is about to be
    # overwritten below. Preserve it in the superseded registry instead of
    # letting it silently fall out of the manifest -- unreachable by lookup
    # (the manifest no longer names it) and unprotected from _prune (only
    # manifest-referenced elf_keys survive pruning) at the same time.
    superseded = _load_superseded(archive_dir)
    prior = manifest.get(identity)
    if prior is not None and prior.get("elf_key") != elf_key:
        bucket = superseded.setdefault(identity, [])
        if not any(s.get("elf_key") == prior.get("elf_key") for s in bucket):
            bucket.append(prior)
            _write_superseded(archive_dir, superseded)
    # A monotonic sequence number, not just the wall-clock "archived_at"
    # string: several archives can land within the same second (bench
    # scripts, or this module's own tests), and archived_at's second
    # resolution can't order those deterministically -- retention below needs
    # an unambiguous "most recent N" to protect.
    next_seq = 1 + max((e.get("seq", 0) for e in manifest.values()), default=0)
    entry = {
        "elf_key": elf_key,
        "identity": identity,
        "seq": next_seq,
        "archived_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        **extra,
    }
    # Keyed by identity so a lookup is one dict access; elf_key kept inside
    # the entry too so pruning can find the file regardless of which key
    # style is used to inspect the manifest by hand.
    manifest[identity] = entry
    _write_manifest(archive_dir, manifest)

    _prune(archive_dir, prefix, manifest, superseded)
    return ArchiveResult(elf_key=elf_key, archived_path=dest, identity=identity,
                          newly_archived=newly_archived)


def _prune(archive_dir: str, prefix: str, manifest: dict, superseded: Optional[dict] = None) -> None:
    """Caps the archive at MAX_ARCHIVED_ELFS files, deleting oldest-by-mtime
    first. Never deletes `<prefix>-latest.elf`, never deletes a file whose
    elf_key is referenced by one of the KEEP_RECENT_ENTRIES most-recently-
    archived manifest entries, and never deletes a file whose elf_key is
    recorded in `superseded` (an older build that shares its fw_build
    identity with a newer one, see SUPERSEDED_NAME) -- those are exactly the
    files that used to be neither reachable nor protected."""
    try:
        entries = sorted(
            (name for name in os.listdir(archive_dir)
             if name.startswith(prefix + "-") and name.endswith(".elf")
             and name != f"{prefix}-latest.elf"),
        )
    except OSError:
        return
    if len(entries) <= MAX_ARCHIVED_ELFS:
        return

    protected_keys = set()
    for entry in sorted(manifest.values(), key=lambda e: e.get("seq", 0), reverse=True)[:KEEP_RECENT_ENTRIES]:
        key = entry.get("elf_key")
        if key:
            protected_keys.add(f"{prefix}-{key}.elf")
    for bucket in (superseded or {}).values():
        for entry in bucket:
            key = entry.get("elf_key")
            if key:
                protected_keys.add(f"{prefix}-{key}.elf")

    full_paths = [os.path.join(archive_dir, name) for name in entries]
    full_paths.sort(key=lambda p: os.path.getmtime(p))  # oldest first

    to_delete_count = len(entries) - MAX_ARCHIVED_ELFS
    deleted = 0
    for p in full_paths:
        if deleted >= to_delete_count:
            break
        if os.path.basename(p) in protected_keys:
            continue
        try:
            os.remove(p)
            deleted += 1
        except OSError:
            continue


def archive_kiln_elf(elf_path: str, fw_build: str, git_commit: Optional[str],
                      source: str) -> ArchiveResult:
    """Archives a just-flashed KilnFW ELF, keyed by `fw_build` (the exact
    string the board's own /api/status reports back as `fw_build` -- see
    esp_app_desc.build_timestamp / build_timestamps_match). `source` is a
    short note (e.g. "flash_firmware" or "flash_firmware:kiln_fw_root
    override") recorded for provenance, not used as a lookup key.

    2026-09-10 fix (opus review round 2, defect D): `fw_build` is
    whitespace-normalized via esp_app_desc.normalize_build_timestamp()
    before being used as the manifest key, matching what
    find_kiln_elf_for_build() now normalizes its lookup key to and what
    build_timestamps_match() already normalized for comparison -- see that
    function's doc comment for why raw __DATE__ strings are not safe to key
    on directly (single-digit-day padding varies)."""
    return _archive(elf_path, kiln_archive_dir(), "KilnCtrl", normalize_build_timestamp(fw_build),
                     extra={"git_commit": git_commit, "source": source})


def archive_safty_elf(elf_path: str, safty_fw_root: str, source: str) -> ArchiveResult:
    """Archives a just-flashed SaftyFW ELF, keyed by its own
    saftyfw_build_info.h identity (commit + build date + build time) --
    SaftyFW has no HTTP API to report a runtime build timestamp independently,
    so this is the same identity stale_check.py already trusts."""
    header_path = os.path.join(safty_fw_root, "build", "saftyfw_build_info.h")
    commit = stale_check._parse_header_define(header_path, "SAFTYFW_GIT_COMMIT")
    build_date = stale_check._parse_header_define(header_path, "SAFTYFW_BUILD_DATE")
    build_time = stale_check._parse_header_define(header_path, "SAFTYFW_BUILD_TIME")
    if not commit:
        raise ValueError(f"elf_archive: could not read SAFTYFW_GIT_COMMIT from {header_path}")
    identity = f"{commit}_{build_date or 'unknown-date'}_{build_time or 'unknown-time'}"
    return _archive(elf_path, safty_archive_dir(), "SaftyFW", identity,
                     extra={"git_commit": commit, "build_date": build_date,
                            "build_time": build_time, "source": source})


def find_kiln_elf_for_build(fw_build: str) -> tuple[Optional[str], str]:
    """Returns (path, message). path is None on no match -- message always
    explains what was searched and, on a miss, how many entries exist so a
    genuine "never archived" case is distinguishable from a manifest bug."""
    archive_dir = kiln_archive_dir()
    manifest = _load_manifest(archive_dir)
    # 2026-09-10 fix (opus review round 2, defect D): normalize the lookup
    # key the same way archive_kiln_elf() now normalizes the stored key --
    # an exact, unnormalized `manifest.get(fw_build)` missed an archived
    # entry whenever the caller's fw_build string's __DATE__ single-digit-day
    # padding differed textually from what was recorded, even though it was
    # the same build (build_timestamps_match() already tolerated exactly
    # this difference; the archive's own key/lookup did not).
    norm_build = normalize_build_timestamp(fw_build)
    entry = manifest.get(norm_build)
    superseded = _load_superseded(archive_dir).get(norm_build, [])
    if entry is None:
        return None, (
            f"no archived ELF found for fw_build={fw_build!r} "
            f"({len(manifest)} entries in {archive_dir}/{MANIFEST_NAME}) -- "
            "this build was never flashed via flash_firmware() since this "
            "mechanism was added, the manifest entry was pruned, or (check "
            f"{archive_dir}/{SUPERSEDED_NAME}) it was superseded by a later "
            "archive call that reused the same fw_build identity"
        )
    path = os.path.join(archive_dir, f"KilnCtrl-{entry['elf_key']}.elf")
    if not os.path.isfile(path):
        return None, (
            f"manifest has an entry for fw_build={fw_build!r} (elf_key={entry['elf_key']}) "
            f"but the file is missing on disk at {path} -- do not guess a substitute"
        )
    message = f"found {path} (archived {entry.get('archived_at')}, commit {entry.get('git_commit')})"
    if superseded:
        other_keys = ", ".join(s.get("elf_key", "?") for s in superseded)
        message += (
            f" -- NOTE: {len(superseded)} other build(s) share this exact fw_build "
            f"identity but were superseded (elf_key(s): {other_keys} in "
            f"{archive_dir}/{SUPERSEDED_NAME}); this happens when a rebuild doesn't "
            "touch the translation unit embedding __DATE__/__TIME__, so fw_build "
            "alone does not uniquely identify content -- if this ELF doesn't match "
            "the panic under investigation, inspect the superseded ones directly"
        )
    return path, message


def find_safty_elf_for_identity(commit: str, build_date: Optional[str] = None,
                                 build_time: Optional[str] = None) -> tuple[Optional[str], str]:
    archive_dir = safty_archive_dir()
    manifest = _load_manifest(archive_dir)
    if build_date and build_time:
        identity = f"{commit}_{build_date}_{build_time}"
        entry = manifest.get(identity)
        if entry:
            path = os.path.join(archive_dir, f"SaftyFW-{entry['elf_key']}.elf")
            if os.path.isfile(path):
                return path, f"found {path} (exact match, archived {entry.get('archived_at')})"
    # Fall back to a commit-only match if exactly one entry has that commit --
    # ambiguous (more than one) is reported loudly rather than picking one.
    candidates = [e for e in manifest.values() if e.get("git_commit") == commit]
    if len(candidates) == 1:
        entry = candidates[0]
        path = os.path.join(archive_dir, f"SaftyFW-{entry['elf_key']}.elf")
        if os.path.isfile(path):
            return path, f"found {path} (commit-only match, archived {entry.get('archived_at')})"
    if len(candidates) > 1:
        return None, (
            f"commit {commit!r} matches {len(candidates)} archived entries with different "
            "build date/time (rebuilt more than once from the same commit) -- pass "
            "build_date/build_time to disambiguate, do not guess"
        )
    return None, (
        f"no archived ELF found for SaftyFW commit={commit!r} "
        f"({len(manifest)} entries in {archive_dir}/{MANIFEST_NAME})"
    )
