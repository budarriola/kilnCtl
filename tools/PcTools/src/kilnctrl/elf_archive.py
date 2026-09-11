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

Retention (current policy -- see the "owner-directed cleanup" note below for
how this superseded the two intermediate versions this docstring also
narrates for their historical context): `_prune` caps each archive directory
at `MAX_ARCHIVED_ELFS` entries (default 60), deleting the oldest-by-
archived_at files first. Never deletes `<prefix>-latest.elf`. A manifest
entry that was actually flashed to a board (`_is_flash_sourced`) is protected
UNCONDITIONALLY regardless of age -- a board flashed weeks ago and only
diagnosed today should not have already lost its ELF, and an OLDER flashed
build can still be the one currently running (OTA/otadata hazard, see
CLAUDE.md). Anything else -- an ordinary local build that was never
confirmed flashed, whether adopted from an orphan file or hand-reconstructed
-- is protected only for `GRACE_PERIOD_HOURS` (default 48) after it was
archived/adopted, then becomes eligible for deletion. Pruning runs after
every archive call, so the directory is bounded on an ongoing basis rather
than needing a separate cron/cleanup step.
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

2026-09-10, opus review round 4 (round 3's fix was a snapshot of the leak,
not the leak -- the hand reconstruction above was already stale by the time
it was committed, since nothing stops `archive_elf.cmake`'s POST_BUILD copy
from depositing the next unregistered file):
  1. Root cause: `archive_elf.cmake` (invoked on every `idf.py build`) copies
     an ELF into this directory keyed only by content hash and never touches
     manifest.json -- so every ordinary build produces a file simultaneously
     unreachable by lookup and unprotected from pruning. Closed from the
     read/write side of this module rather than the cmake side (no reliable
     way to compute the runtime-reported `fw_build` identity at cmake time):
     `adopt_orphaned_kiln_elfs()` scans the archive directory for
     `KilnCtrl-*.elf` files the manifest/superseded registry doesn't
     reference, recovers each one's identity by scanning the ELF's own bytes
     for its embedded `esp_app_desc_t` (`esp_app_desc.scan_elf_for_app_descs`
     -- pyelftools is not available in this environment, confirmed 2026-09-10,
     so this scans for the struct's magic word directly rather than parsing
     ELF section headers), and registers it. Called automatically at the top
     of every `archive_kiln_elf()` call, so the backlog self-heals on the
     next flash and cannot grow unboundedly between fixes again. Also
     callable directly for one-off maintenance; used once, 2026-09-10, to
     adopt the 8 orphans measured live at review time (0 left unresolved).
  2. `_prune()` used to protect only the `KEEP_RECENT_ENTRIES` (10)
     most-recently-archived manifest entries, not all of them -- measured
     live: 32 of 42 manifest entries were unprotected, one `_prune` call
     away from deleting a file a real lookup could still resolve to.
     Protection is now unconditional for every manifest- or
     superseded-referenced elf_key, regardless of age -- a manifest entry
     exists precisely so a build can be found again later, and there is no
     principled point at which that stops being true. This means the
     MAX_ARCHIVED_ELFS cap can no longer be enforced once registered content
     alone exceeds it (already true live, ~1.3 GB across 68 files against a
     60-file cap): `_prune` now says so loudly instead of silently leaving
     the directory over cap or, worse, silently deleting something
     reachable. Shedding registered identities is a separate, deliberate
     decision for a human, not a side effect of an ordinary archive call.
  3. `_load_manifest`'s raw/normalized-key migration used to drop the
     lower-`seq` duplicate with no record at all when a collision was found
     -- reproducing, by a different mechanism, the exact "unregistered
     producer" class defect 1 above closes. The dropped entry is now
     recorded into `superseded.json` (whichever of the two loses, regardless
     of dict iteration order -- an earlier draft of this fix only handled
     one direction of that comparison). A manifest whose top-level JSON
     value isn't an object, or whose individual entry isn't one, is now a
     loud warning instead of either a silent `{}` (indistinguishable from
     "no manifest yet") or an uncaught `AttributeError` that took down every
     caller over one malformed row.

2026-09-10/11, owner-directed cleanup (round 4's fix made `_prune` honest
about being unable to enforce the cap -- it did not make the cap
enforceable, because "protect every registered identity forever" and "cap
at 60" are contradictory once registered content alone exceeds 60, which was
already true: measured live, only 2 of 47 manifest entries (source
`flash_firmware`/`flash_firmware(kiln_fw_root=...)`) were ever actually
flashed to a board; the other 45, plus all 21 superseded entries, were
`archive_elf.cmake` POST_BUILD deposits from ordinary local builds that were
never flashed, later given manifest entries by `adopt_orphaned_kiln_elfs()`/
the round-3 hand reconstruction purely so they would not be silently
unreachable -- "registered" was never the same claim as "flashed"):
  1. Root cause fixed at the source: `archive_elf.cmake` no longer deposits
     a permanent hash-keyed copy on every build at all (see that file) -- it
     only refreshes the `KilnCtrl-latest.elf` convenience pointer, which is
     overwritten in place and never accumulates. Going forward, the only way
     a new entry lands in this directory is a confirmed flash
     (`archive_kiln_elf`/`archive_safty_elf`) or a one-off manual
     `adopt_orphaned_kiln_elfs()` maintenance call against a pre-existing
     backlog -- so the volume this module has to manage is now bounded by
     how often the board is actually flashed, not how often it is built.
  2. Retention is now a real, provenance-based policy instead of
     "protect everything registered, forever":
       - `_is_flash_sourced()` recognizes an entry as an actual flash
         (`source` starting with `"flash_firmware"` or `"debug_program"` --
         the two call sites in mcp_server_flash.py/mcp_server_debug.py) and
         such entries are protected from pruning UNCONDITIONALLY, regardless
         of age -- this is the "keep flashed images generally" rule, and it
         deliberately does not prefer the newest: an older flashed build can
         still be the one a board is running (`flash_firmware()` writes only
         the `factory` partition and never touches `otadata`, so an OTA that
         pointed the boot target at `ota_0`/`ota_1` leaves the board running
         an older flash indefinitely -- see CLAUDE.md's flash/OTA section).
       - Anything else (an ordinary local build, whether adopted from an
         orphan file or hand-reconstructed) is protected only for
         `GRACE_PERIOD_HOURS` (48) after it was archived/adopted -- long
         enough that a build someone is actively mid-debug on survives
         several idle hours, short enough that it does not re-create the
         original hoarding problem now that new non-flash entries can only
         come from an explicit maintenance call, not an automatic per-build
         one.
       - Past its grace window, a non-flash entry is eligible for deletion,
         oldest-by-archived_at first, until the directory is back at
         `MAX_ARCHIVED_ELFS` -- and its manifest/superseded entry is removed
         in the same pass, so a pruned file never leaves behind a dangling
         "found ... but the file is missing on disk" lookup result.
     This makes the cap meaningful again under ordinary operation: the set
     of unconditionally-protected entries is now bounded by how often the
     board is flashed (small), not by every local build ever adopted. The
     loud "cap not being enforced" warning (round 4, defect 4) is kept for
     the case that no longer needs to be hypothetical -- true flash volume
     alone exceeding the cap -- since that is still a deliberate human
     decision, not something `_prune` should paper over by deleting a
     flashed image.
  3. One-time reclaim (2026-09-10/11): of the 68 files on disk (47 manifest
     + 21 superseded entries, 0 true orphans -- round 4's adoption pass had
     already run), only 2 manifest entries were flash-sourced. The board's
     own currently-running `fw_build` was read live (read-only) and
     confirmed to match one of those two (`KilnCtrl-16bced646ac0.elf`,
     entry `Sep 10 2026 15:45:13`, `source: flash_firmware`) via
     `find_crash_elf()` before anything was deleted. The other 66 files
     (45 non-flash manifest entries + 21 non-flash superseded entries) were
     never-flashed local builds -- none within the new 48h grace window --
     and were deleted along with their manifest/superseded entries,
     reclaiming ~1.3 GB.
"""

from __future__ import annotations

import calendar
import hashlib
import json
import os
import re
import shutil
import time
from dataclasses import dataclass, asdict
from typing import Optional

from . import stale_check
from .esp_app_desc import normalize_build_timestamp, scan_elf_for_app_descs

MAX_ARCHIVED_ELFS = 60
# How long a non-flash-sourced entry (an ordinary local build, adopted or
# hand-reconstructed rather than ever actually flashed) is protected from
# pruning purely because it is recent -- see the module docstring's
# "owner-directed cleanup" section. A flash-sourced entry (see
# _is_flash_sourced) is protected unconditionally, regardless of age, and
# does not use this window at all.
GRACE_PERIOD_HOURS = 48
MANIFEST_NAME = "manifest.json"
# source strings recorded by the two real flash call sites
# (mcp_server_flash.py's archive_kiln_elf call and mcp_server_debug.py's
# archive_safty_elf call for debug_program(peer="pico")), including the
# kiln_fw_root worktree-override variant ("flash_firmware:kiln_fw_root
# override" / "flash_firmware(kiln_fw_root=...)"). Anything else --
# "adopted:orphan-scan", "reconstructed-from-elf-appdesc-...",
# "UNTRUSTED-REPAIRED", a test fixture string -- is a local build that was
# never confirmed flashed to a board, not a build the owning tool merely
# forgot to label.
_FLASH_SOURCE_PREFIXES = ("flash_firmware", "debug_program")


def _is_flash_sourced(entry: dict) -> bool:
    source = entry.get("source") if isinstance(entry, dict) else None
    return isinstance(source, str) and source.startswith(_FLASH_SOURCE_PREFIXES)


def _parse_archived_at(entry: dict) -> Optional[float]:
    """Returns the entry's archived_at as a UTC unix timestamp, or None if
    absent/unparseable -- callers must treat that as "cannot prove this is
    recent", not as "very old" or "very new"."""
    stamp = entry.get("archived_at") if isinstance(entry, dict) else None
    if not isinstance(stamp, str):
        return None
    try:
        return calendar.timegm(time.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ"))
    except ValueError:
        return None
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


def _load_manifest(archive_dir: str, *, persist_migration: bool = True) -> dict:
    """Loads and normalizes the manifest. `persist_migration` controls
    whether a raw/normalized-key collision that drops an entry (see below)
    is recorded into superseded.json immediately -- callers that already
    hold archive_dir context and intend to write anyway (e.g. `_archive`)
    can pass True (the default); it is also safe from a pure read path
    (`find_kiln_elf_for_build`), which is exactly where this needs to run
    since that is the only place a stale on-disk manifest otherwise gets
    read at all."""
    path = os.path.join(archive_dir, MANIFEST_NAME)
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(raw, dict):
        # 2026-09-10 (opus review round 3, defect 3): a corrupt manifest
        # whose top-level value isn't an object at all previously fell
        # through to the same `return {}` as "file absent" -- indistinguishable
        # from a fresh archive. Make the distinction loud instead of silent;
        # callers still get an empty (usable) manifest rather than a crash,
        # since a hard failure here would block every subsequent archive/
        # lookup call over one corrupt file.
        print(f"elf_archive: WARNING -- {path} does not contain a JSON object "
              f"(got {type(raw).__name__}); treating as empty. This manifest is "
              "corrupt and needs manual inspection -- entries it held (if any) "
              "are not being deleted, just not read.")
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
    dropped: list = []  # entries that lost the raw/normalized collision below
    for key, entry in raw.items():
        if not isinstance(entry, dict):
            # 2026-09-10 (opus review round 3, defect 3): a malformed
            # individual entry (not a dict) previously raised AttributeError
            # out of this function on the very next `.get()` call, taking
            # down every caller (archive AND lookup) over one bad row.
            # Skip it loudly instead -- it is neither reachable nor
            # protected, same as an unregistered orphan file, but that is a
            # narrower, more honest failure than refusing to read the whole
            # manifest.
            print(f"elf_archive: WARNING -- manifest entry {key!r} in {path} "
                  f"is not an object (got {type(entry).__name__}); skipping it.")
            continue
        norm_key = normalize_build_timestamp(key)
        prior = manifest.get(norm_key)
        if prior is None:
            manifest[norm_key] = entry
            continue
        # A raw/normalized collision: keep whichever of the two has the
        # higher seq, record the OTHER one as dropped -- regardless of which
        # one (the earlier-seen `prior` or the just-read `entry`) that turns
        # out to be. 2026-09-10 (opus review round 4, defect 3 correction):
        # an earlier version of this branch only handled the case where
        # `entry` (processed second, in dict order) lost -- if `prior`
        # (processed first) had the LOWER seq instead, `manifest[norm_key] =
        # entry` below silently overwrote it with no record at all, which is
        # the exact silent-drop this fix exists to close. Iteration order
        # must not change which duplicate gets recorded.
        if prior.get("seq", 0) > entry.get("seq", 0):
            dropped.append((norm_key, entry))
            continue  # keep `prior`, already in manifest
        dropped.append((norm_key, prior))
        manifest[norm_key] = entry
    if dropped and persist_migration:
        superseded = _load_superseded(archive_dir)
        changed = False
        for norm_key, entry in dropped:
            bucket = superseded.setdefault(norm_key, [])
            if not any(s.get("elf_key") == entry.get("elf_key") for s in bucket):
                bucket.append(entry)
                changed = True
        if changed:
            _write_superseded(archive_dir, superseded)
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


def adopt_orphaned_kiln_elfs(archive_dir: str) -> tuple[int, list[str]]:
    """Scans `archive_dir` for `KilnCtrl-<key>.elf` files that neither the
    manifest nor the superseded registry references, and registers each one
    it can positively identify -- closing defect 1 (2026-09-10, opus review
    round 4): `archive_elf.cmake`'s POST_BUILD step (see that file) copies
    a freshly-linked ELF into this directory on every ordinary `idf.py
    build`, keyed only by content hash, and never touches manifest.json.
    Every such file is deposited simultaneously unreachable by lookup (no
    manifest entry) and unprotected from `_prune` (only manifest/superseded-
    referenced files survive pruning) -- confirmed live: 7 orphans measured
    against a 42-entry manifest, one of which (`ca44736c7d9b`) is the exact
    ELF a stack-budget measurement was taken against.

    Identity for an orphan is recovered from the ELF's own embedded
    `esp_app_desc_t` (`esp_app_desc.scan_elf_for_app_descs` -- see that
    function for why this scans for the struct's magic word rather than
    parsing ELF section headers). An orphan that yields zero or more than
    one distinct timestamp is left alone and reported rather than guessed at
    -- adopting it under a fabricated identity would be strictly worse than
    leaving it unregistered, since a wrong manifest entry produces a
    confident, WRONG symbolization instead of an honest "not found".

    Called automatically from `_archive()` (KilnFW path only -- SaftyFW has
    no embedded HTTP-reportable build timestamp to scan for) before every
    ordinary archive, so the set of orphans can only shrink over time absent
    a cmake/toolchain change. Also callable directly for one-off maintenance
    against the existing backlog.

    2026-09-10/11: `archive_elf.cmake` no longer deposits an unregistered
    copy on every build at all (see that file), so this should find nothing
    to do on an ordinary run from here on -- it stays in place as the
    self-healing path for any pre-existing backlog or an unanticipated
    future producer, per its own defense-in-depth rationale above, not
    because it is still needed for the common case. An entry this function
    adopts is registered so it is *reachable*, not so it is *protected
    forever*: `_prune`'s retention policy treats an adopted entry the same
    as any other non-flash-sourced one (see `_is_flash_sourced`) -- eligible
    for deletion once it ages out of `GRACE_PERIOD_HOURS`, since adoption
    recovers an identity, it does not establish that the board was ever
    flashed with it.

    Returns (adopted_count, still_unresolved_filenames)."""
    _guard_against_test_write(archive_dir)
    prefix = "KilnCtrl"
    try:
        names = sorted(
            name for name in os.listdir(archive_dir)
            if name.startswith(prefix + "-") and name.endswith(".elf")
            and name != f"{prefix}-latest.elf"
        )
    except OSError:
        return 0, []

    manifest = _load_manifest(archive_dir)
    superseded = _load_superseded(archive_dir)
    known_keys = {e.get("elf_key") for e in manifest.values() if isinstance(e, dict)}
    for bucket in superseded.values():
        known_keys.update(e.get("elf_key") for e in bucket if isinstance(e, dict))

    key_re = re.compile(rf"^{prefix}-([0-9a-f]{{12}})\.elf$")
    orphans = []
    for name in names:
        m = key_re.match(name)
        if m and m.group(1) not in known_keys:
            orphans.append((name, m.group(1)))

    if not orphans:
        return 0, []

    adopted = 0
    unresolved: list[str] = []
    manifest_dirty = False
    superseded_dirty = False
    for name, elf_key in orphans:
        elf_path = os.path.join(archive_dir, name)
        try:
            descs = scan_elf_for_app_descs(elf_path)
        except OSError as exc:
            print(f"elf_archive: WARNING -- could not scan orphan {name} for identity: {exc}")
            unresolved.append(name)
            continue
        if len(descs) != 1:
            print(f"elf_archive: WARNING -- orphan {name} yielded "
                  f"{len(descs)} candidate build identities (need exactly 1); "
                  "leaving it unregistered rather than guessing.")
            unresolved.append(name)
            continue
        identity = normalize_build_timestamp(descs[0].build_timestamp)
        entry = {
            "elf_key": elf_key,
            "identity": identity,
            "seq": 1 + max((e.get("seq", 0) for e in manifest.values()), default=0),
            "archived_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(os.path.getmtime(elf_path))),
            "git_commit": None,
            "source": "adopted:orphan-scan",
        }
        prior = manifest.get(identity)
        if prior is None:
            manifest[identity] = entry
            manifest_dirty = True
        elif prior.get("elf_key") == elf_key:
            # Already the live entry's key by some other path -- nothing to do.
            pass
        else:
            # Identity collision with an existing, different, current entry:
            # don't disturb which one is "live" based only on a directory
            # scan -- record the orphan as superseded so it is reachable via
            # the superseded registry and protected from pruning, same as
            # any other identity collision (see _archive's own handling).
            bucket = superseded.setdefault(identity, [])
            if not any(s.get("elf_key") == elf_key for s in bucket):
                bucket.append(entry)
                superseded_dirty = True
        adopted += 1
        known_keys.add(elf_key)

    if manifest_dirty:
        _write_manifest(archive_dir, manifest)
    if superseded_dirty:
        _write_superseded(archive_dir, superseded)
    if adopted:
        print(f"elf_archive: adopted {adopted} orphaned ELF(s) in {archive_dir} "
              "into the manifest/superseded registry (archive_elf.cmake's "
              "POST_BUILD copy never registers what it writes).")
    return adopted, unresolved


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

    if prefix == "KilnCtrl":
        # 2026-09-10 (opus review round 4, defect 1): adopt any orphans left
        # behind by archive_elf.cmake's POST_BUILD copy (which never
        # registers what it writes) before this call adds its own entry --
        # keeps the backlog from growing between flashes and self-heals the
        # existing one over time. See adopt_orphaned_kiln_elfs's docstring.
        adopt_orphaned_kiln_elfs(archive_dir)

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
    """Caps the archive at MAX_ARCHIVED_ELFS files where it safely can,
    deleting oldest-by-archived_at first. Never deletes `<prefix>-latest.elf`.

    Protection is provenance-based, not "everything registered, forever"
    (see the module docstring's "owner-directed cleanup" section for the
    incident this replaces):
      - A manifest entry that is flash-sourced (`_is_flash_sourced` --
        actually flashed via flash_firmware()/debug_program(), the two real
        producers) is protected UNCONDITIONALLY, regardless of age. An older
        flashed build can still be the one a board is running (OTA can point
        the boot target at an image `flash_firmware()` never touches -- see
        CLAUDE.md's flash/OTA section), so age is not a safe signal here.
      - Any other manifest or superseded entry (an ordinary local build,
        whether adopted from an orphan file or hand-reconstructed -- never
        confirmed flashed) is protected only for GRACE_PERIOD_HOURS after it
        was archived/adopted. An entry with no parseable `archived_at` is
        treated as NOT recent (fails safe toward eligible-for-deletion,
        never toward permanent protection by default) -- see
        `_parse_archived_at`.
    A file whose elf_key is protected by either rule is never deleted.
    Deleting a file also removes its manifest/superseded entry (and
    rewrites those to disk) so pruning never leaves a manifest entry whose
    file is missing -- a lookup that later needs that identity should see
    a clean "no archived ELF found", not "the file is missing on disk".

    This makes the cap meaningful under ordinary operation: the
    unconditionally-protected set is now bounded by how often the board is
    actually flashed, not by every local build that was ever built or
    adopted. If flash-sourced entries alone already exceed the cap, this
    function says so loudly rather than deleting a flashed image to make
    room -- that is still a deliberate human decision (see the warning
    below), not something to be papered over."""
    try:
        entries = [
            name for name in os.listdir(archive_dir)
            if name.startswith(prefix + "-") and name.endswith(".elf")
            and name != f"{prefix}-latest.elf"
        ]
    except OSError:
        return

    # Reverse-index elf_key -> (source dict, container, container-key) so a
    # deleted file's entry can be removed from whichever of manifest/
    # superseded it lives in.
    key_info: dict[str, tuple[dict, bool, float]] = {}
    # value: (entry, is_flash_sourced, archived_at-or-None)
    for identity, entry in manifest.items():
        if not isinstance(entry, dict):
            continue
        key = entry.get("elf_key")
        if key:
            key_info.setdefault(key, (entry, _is_flash_sourced(entry), _parse_archived_at(entry)))
    for identity, bucket in (superseded or {}).items():
        for entry in bucket:
            key = entry.get("elf_key") if isinstance(entry, dict) else None
            if key and key not in key_info:
                key_info[key] = (entry, _is_flash_sourced(entry), _parse_archived_at(entry))

    if len(entries) <= MAX_ARCHIVED_ELFS:
        return

    now = time.time()
    grace_seconds = GRACE_PERIOD_HOURS * 3600

    def _elf_key_of(name: str) -> Optional[str]:
        stem = name[len(prefix) + 1:-len(".elf")]
        return stem or None

    protected: set[str] = set()
    eligible: list[tuple[float, str]] = []  # (archived_at-or-mtime, filename)
    for name in entries:
        key = _elf_key_of(name)
        info = key_info.get(key) if key else None
        if info is not None:
            _entry, flash_sourced, archived_at = info
            if flash_sourced:
                protected.add(name)
                continue
            age_ok = archived_at is not None and (now - archived_at) < grace_seconds
            if age_ok:
                protected.add(name)
                continue
            sort_key = archived_at if archived_at is not None else 0.0
        else:
            # Unregistered file with no manifest/superseded entry at all --
            # should not occur for KilnCtrl once adopt_orphaned_kiln_elfs()
            # has run (called from _archive before this), but fail toward
            # "eligible for deletion" rather than "protected forever" if it
            # somehow does (e.g. SaftyFW, which has no adoption path).
            try:
                sort_key = os.path.getmtime(os.path.join(archive_dir, name))
            except OSError:
                sort_key = 0.0
        eligible.append((sort_key, name))

    eligible.sort()  # oldest first

    to_delete_count = len(entries) - MAX_ARCHIVED_ELFS
    deleted_names: list[str] = []
    for _sort_key, name in eligible:
        if len(deleted_names) >= to_delete_count:
            break
        try:
            os.remove(os.path.join(archive_dir, name))
            deleted_names.append(name)
        except OSError:
            continue

    if deleted_names:
        deleted_keys = {_elf_key_of(n) for n in deleted_names}
        manifest_dirty = False
        for identity in list(manifest.keys()):
            entry = manifest[identity]
            if isinstance(entry, dict) and entry.get("elf_key") in deleted_keys:
                del manifest[identity]
                manifest_dirty = True
        if manifest_dirty:
            _write_manifest(archive_dir, manifest)
        if superseded is not None:
            superseded_dirty = False
            for identity in list(superseded.keys()):
                bucket = [e for e in superseded[identity]
                          if not (isinstance(e, dict) and e.get("elf_key") in deleted_keys)]
                if len(bucket) != len(superseded[identity]):
                    superseded_dirty = True
                if bucket:
                    superseded[identity] = bucket
                else:
                    del superseded[identity]
            if superseded_dirty:
                _write_superseded(archive_dir, superseded)
        print(f"elf_archive: pruned {len(deleted_names)} never-flashed, "
              f"past-grace-window {prefix} ELF(s) from {archive_dir} "
              f"(cap {MAX_ARCHIVED_ELFS}).")

    if len(deleted_names) < to_delete_count:
        # A cap that is silently unenforceable is worse than no cap: callers
        # still believe MAX_ARCHIVED_ELFS is a real ceiling. This can now
        # only happen when flash-sourced entries (unconditionally protected)
        # or very recent non-flash entries (within GRACE_PERIOD_HOURS) alone
        # exceed the cap -- both are deliberate protections, not a bug being
        # masked, but disk usage is unbounded until a human decides some of
        # those identities are safe to retire and removes them explicitly.
        still_over = len(entries) - len(deleted_names) - MAX_ARCHIVED_ELFS
        print(f"elf_archive: WARNING -- {archive_dir} holds {len(entries) - len(deleted_names)} "
              f"{prefix} ELFs against a cap of {MAX_ARCHIVED_ELFS} ({still_over} over) after "
              f"pruning {len(deleted_names)} eligible file(s); the rest are protected because "
              "they were actually flashed to a board (unconditional) or archived/adopted within "
              f"the last {GRACE_PERIOD_HOURS}h grace window. The cap is NOT being enforced. This "
              "is a deliberate correctness-over-cap tradeoff (see _prune's docstring), not a bug "
              "being masked.")


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
