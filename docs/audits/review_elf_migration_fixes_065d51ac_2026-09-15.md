# Adversarial review: 065d51ac (elf_archive migration fixes M1/M2/L1-L4)

Date: 2026-09-15. This was a read-only review. The fixes being reviewed come from
`docs/audits/review_elf_archive_fixes_a6f4a624_2026-09-15.md`.
Line numbers refer to `tools/PcTools/src/kilnctrl/elf_archive.py` at 065d51ac.

## Verification performed

- Clean worktree at origin/main (which was 065d51ac) under `C:\wt\`, using the main-tree `.venv` interpreter.
  `test_elf_archive.py`, `test_get_fw_version_provenance.py`, `test_flash_provenance.py`,
  `test_flash_board_pinning.py`, `test_flash_firmware_verify.py` and `test_capture_pool_provenance.py`:
  **152 passed**.
- **New tests against the parent's source** (the parent's `src/` plus the new `tests/`): 14 failed and 44 passed.
  One failure, `test_cmake_outdir_matches_kiln_archive_dir`, is an artifact of the extracted tree, not a finding. The new tests that
  **still pass against the old code**, and so cannot detect the regression they are named for, are:
  - `test_migrated_flash_sourced_entry_survives_prune_despite_ancient_archived_at`. The old code moved
    `manifest.json` whenever the destination had none. This test's destination has none, so the old code passes it.
    It never exercises the M1 collision case, where a new manifest already exists.
  - All five `test_negative_*` tests (the `shutil.move` one, `..._stranded_on_collision`,
    `..._migrated_flashed_entry_gets_pruned`, `..._saftyfw_..._unreachable` and `..._provenance_path_check_...`).
    Each rebuilds the "old" state by hand inside the test and asserts on that hand-built state. None of them runs the old code.
  - `test_guard_refuses_a_write_to_the_canonical_provenance_path_directly` (L4). This is expected, because the guard already existed.
- **Negative test (restored by hand, byte-compared, empty `git status`):** I replaced
  `merged_keys = _merge_legacy_manifest(...)` with `set()`. Five tests then failed: the move test, both merge tests,
  the prune-survival test and the SaftyFW lookup test. So against *this* code the prune-survival
  test does catch removal of the merge. It just doesn't reproduce the original defect.
- The worktree was removed.

## Findings

### M1: `_archive()` still writes the manifest outside the lock, so a merge can be lost after its legacy source is deleted
`elf_archive.py:986` (migrate, locked) and `:1003-1031` (load, supersede, write the manifest, **unlocked**).
The lock only covers `migrate_legacy_archive`. The read-modify-write race that the `_archive_lock` docstring
(`:691-705`) says it closes is still open, and the commit message's L2 claim ("the whole migrate+merge
critical section is serialized") does not hold for the part that matters.

Failure scenario:
1. Process A (a flash) runs `_archive`. Its migrate call finds nothing and returns, and then A reads `manifest.json` (`:1003`).
2. Meanwhile a legacy `build/elf_archive/manifest.json` appears. The most likely source is a stale server that
   is still writing the old layout, which CLAUDE.md's "Stale-server self-announcing" says really happens.
3. Process B (`find_crash_elf` running on the lookup path, `:1234`) takes the lock, merges the legacy entries, writes
   `manifest.json`, and then **deletes the legacy manifest** (`:926-934`).
4. A writes its stale copy (`:1031`) and clobbers B's merge.

The flashed legacy entry now exists nowhere. Its ELF is later adopted as `adopted:orphan-scan` with an mtime-derived
`archived_at`, which is exactly the provenance loss M1 was meant to fix, and this time it can't be recovered.

Fix: hold `_archive_lock` across the whole of `_archive` (migrate, adopt, load, write, prune). Take the lock
re-entrantly, or split migrate into a locked and an unlocked variant.

### M2: the lock-steal timeout deletes another holder's lock, and there is no age check
`:708-731`. When the wait exceeds `timeout_s`, the loser proceeds with `fd=None`, but its `finally` still
calls `os.remove(lock_path)` unconditionally. A crashed holder's lock and a live-but-slow holder's lock look the same,
because the code checks neither PID nor mtime.

- **POSIX:** the stealer removes a live holder's lock, so a third process gets in while the first is still inside
  the critical section. That makes three writers.
- **Windows:** `os.remove` on a file another process still holds open (the holder keeps `fd` open) fails with
  `PermissionError`, which is swallowed here. The live holder is therefore safe on Windows, but the stealer still runs
  its merge concurrently with that holder.

For the **stale lock after a crash** case, there is no deadlock. The OS closes a dead process's handle, so the next caller waits
30 s, proceeds, and removes the orphan lock, which heals it. The cost is a one-time **30 s stall inside
`flash_firmware()` or inside a crash lookup** (`find_crash_elf`). The
critical section normally takes milliseconds, so a lock-file mtime older than a few seconds is a much better stale signal than
30 s of polling.

No test covers `_archive_lock` at all: not contention, not stale-lock takeover, not the timeout.

### M3: a lookup now mutates the archive and can raise
`:1234` and `:1281`. `find_kiln_elf_for_build` and `find_safty_elf_for_identity` call `migrate_legacy_archive` without a
try/except. Once a legacy directory exists (the `isdir` check at `:855` returns early otherwise), link and remove errors
are caught, but the `os.makedirs` in `_archive_lock` and
`_write_manifest`/`_write_superseded` (`:532-560`) are not. With a legacy directory present, a read-only or locked
archive directory (for example a OneDrive sync lock) turns a crash-symbolization lookup into an exception, where before it
returned a clean "no match". The same call also stalls for up to 30 s behind a stale lock, as described in M2.

Fix: wrap the lookup-side migrate in `try/except Exception` with a warning, the same way `flash_firmware`
already wraps `migrate_legacy_provenance`.

### L1: an identical-content name collision is never cleaned up, so L1's repeating warning survives
`:878-883` and `:958-962`. ELF names are content-addressed (`<prefix>-<sha12>.elf`), so a `FileExistsError`
means the legacy copy is byte-identical to the new one. It is still kept "for a human to reconcile", which means
`remaining` is never empty. The legacy directory then stays, and the "left N legacy entry(ies) unmoved" line prints on
**every lookup and every flash** from now on. That repeating line is what L1 was meant to remove. It is safe to delete the legacy
copy after `_sha256_key` matches.

### L2: legacy entries whose ELF file is missing are merged as dangling manifest entries
`:776-788`. The merge copies every legacy manifest entry without checking that
`legacy_dir/<prefix>-<elf_key>.elf` (or the destination file) exists. If a link fails with a non-`FileExistsError`
`OSError` (`:884-886`), the ELF stays in the legacy directory. The legacy manifest is still deleted, because the elf_key sits in
`merged_keys`, which is computed from the manifest alone and not from file moves. The new manifest then names a file
that is not in `archive_dir`, and a lookup reports a hit on a missing file. A later run does link the ELF across, so no data is
lost, but the "cleanup never deletes a file that isn't fully migrated" invariant is only half true. It covers manifest
entries but not the files they reference.

### L3: the merge keeps legacy `seq` values as they are
`:778-780`. Legacy `seq` numbers can duplicate or interleave with new ones. `_archive`'s `max+1` stays monotonic,
but anything that orders by `seq` gets an arbitrary order for merged entries. This is a cosmetic or retention-ordering issue only,
because flash-sourced entries are protected regardless.

### L4: KilnFW lookup-time migration (M2) has no test
Only the SaftyFW lookup path has one (`test_saftyfw_legacy_manifest_is_merged_and_findable`, which does fail against the old
code). Nothing calls `find_kiln_elf_for_build` against a populated legacy directory.

### L5: provenance migration ignores a newer legacy record once the new file exists
`elf_archive.py:338-370` and `mcp_server_info.py` `_read_last_flash_warning`. If both files exist (for example a
stale server wrote to the old path after a new server migrated), `migrate_legacy_provenance` returns False and the
reader uses only the new path. A newer refused-flash record at the legacy path is silently ignored. This is by design ("never
guess"), but it deserves a warning line.

## Checked and OK

- **Provenance and dates are preserved:** merged entries are copied verbatim (`source`, `archived_at`). If an identity
  collides, the legacy entry goes to `superseded.json`, and `_prune` applies `_is_flash_sourced` to superseded
  entries too (`:1073-1077`). So a flashed legacy entry that loses a collision is still protected. The negative test confirms
  that a migrated flashed entry survives `_prune` under the new code.
- **Manifest cleanup is fail-safe:** it re-reads the raw JSON and requires every elf_key to be in `merged_keys`, so a
  malformed or dropped entry keeps the file.
- **`os.link` semantics:** `os.link` raises `FileExistsError` on both Windows (NTFS `CreateHardLink`) and POSIX,
  and never overwrites. Link then remove on the same volume preserves content and mtime. A failed remove leaves
  a harmless duplicate. On FAT or exFAT, or across volumes, link fails with an `OSError` and the file stays where it is (see L2).
- `os.replace` is used only for this module's own tmp-to-manifest writes, which is correct.
