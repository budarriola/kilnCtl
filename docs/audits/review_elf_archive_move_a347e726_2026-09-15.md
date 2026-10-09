# Review: ELF archive moved out of build/ (a347e726), 2026-09-15

Read-only review of `a347e726`. Nothing in production code was edited.

## Summary

The core move is right. `kiln_archive_dir()`/`safty_archive_dir()`, `_canonical_archive_dirs()`
and `archive_elf.cmake`'s OUTDIR (set in `firmware/KilnFW/CMakeLists.txt`) all agree on the new
sibling path. Every Python reader (`find_crash_elf`, `find_kiln_elf_for_build`,
`find_safty_elf_for_identity`, `_prune`, orphan adoption, `debug_program(peer=pico)`'s
`archive_safty_elf`) goes through those two functions, so no code reader is left on the old path. No
`check_*.ps1`, `firmware/*/tools` script or SaftyFW CMake file mentions `elf_archive`. Prune and
retention code was not touched by the diff. The remaining problems are an adjacent file that was left
in `build/`, stale docs, and one test that cannot fail.

## Findings (by severity)

### M1. `flash_provenance.json` is still inside build/ and still gets wiped with it
`tools/PcTools/src/kilnctrl/mcp_server_flash.py:603` still writes
`os.path.join(build_dir, "flash_provenance.json")`. The commit message says the incident lost
"manifest.json, flash_provenance.json, and the archived ELF all together", but only the archive was
moved. **Scenario:** flash, then `idf.py fullclean` (or a fresh configure), then a panic. The ELF can
now be found, but the record of which git state/dirty set was flashed is gone again, and
`read_provenance_json()` returns None. It is also per-tree: with `kiln_fw_root` it lives in the
worktree and is deleted along with it (the docstring near :545 says so). This is the same bug class
that was just fixed, left one file over.

### M2. Docs still point at the old path
- `CLAUDE.md:318` ("Firmware gotchas") still says `build/elf_archive/KilnCtrl-<hash>.elf`. Every
  session reads this file. Anyone who symbolizes by hand looks in the old dir, which now holds only a
  stale 19 MB `KilnCtrl-latest.elf` (see L1). That copy is exactly the "most recently built, not
  running" ELF the gotcha warns against.
- `firmware/KilnFW/docs/BRINGUP_HAZARDS.md:66`: same old path.
- The `tools/PcTools/src/kilnctrl/elf_archive.py` module docstring, lines 7, 14, 21 and 260, still
  describes `firmware/<KilnFW|SaftyFW>/build/elf_archive/` as the canonical location. Only the
  function docstrings were updated.
- `docs/MCP_SERVERS.md` has no `elf_archive` mention, so nothing there needs changing.
- Older audits (`firing_history_stack_overflow_2026-09-08.md:41`,
  `review_autosave_slot_fix_a93ee77b_2026-09-15.md:122`) are historical records. Leave them.

### L1. No migration: old archive contents are silently orphaned
Nothing reads `build/elf_archive/` any more, and nothing moves it. On this machine the loss is nil:
`firmware/KilnFW/build/elf_archive/` holds only `KilnCtrl-latest.elf` (10:57, a convenience
pointer), because the earlier wipe had already destroyed the real entries. SaftyFW has no old dir.
**Scenario:** on any other clone or long-lived worktree whose `build/elf_archive` still holds flashed
entries, `find_crash_elf` reports "no match" for a build it archived, with no hint that the entry is
sitting in the old directory. A one-shot move, or even a warning when the old dir exists, would close
this. The stale old-path `latest.elf` should also be deleted so hand symbolization cannot pick it up
(M2).

### L2. The canonical archive is empty right now
`firmware/KilnFW/elf_archive/manifest.json` is `{}`, and `firmware/SaftyFW/elf_archive/` does not
exist. The images currently running on the boards cannot be found by `find_crash_elf` until each is
reflashed through the tools, after the kilnctrl server is restarted so it runs the new code. If one
of them panics before then, lookup fails loud (correct), but it cannot help.

### L3. `test_archived_elf_survives_simulated_build_wipe` is vacuous
`tools/PcTools/tests/test_elf_archive.py`, inside `ArchiveDirSurvivesBuildWipeTest`: the test
patches `kiln_archive_dir` to return a hand-built *sibling* path and then deletes `build/`. It never
calls the production path function, so it passes against the pre-fix code too. It is a test of a
mirror, not of production. The two path-shape tests (`test_kiln_archive_dir_not_inside_build`,
`test_safty_archive_dir_not_inside_build`) do call the real functions and would fail on the old code,
so the fix is covered. The "negative-tested against production code" claim can only hold for those
two. Also, nothing asserts that CMakeLists.txt's `${CMAKE_CURRENT_LIST_DIR}/elf_archive` matches
`kiln_archive_dir()`. That pairing is enforced only by comments.

### L4. `_canonical_archive_dirs()` still duplicates the path literal
`elf_archive.py:301-310` rebuilds both paths by hand, on purpose, so the pytest guard keeps working
when tests monkeypatch the dir functions. That is a valid reason, but it leaves a second copy of the
contract, which is the very pair that broke in this commit. No test directly asserts that
`_canonical_archive_dirs()` equals the normalized unpatched `kiln_archive_dir()` and
`safty_archive_dir()`. A cheap equality test would pin it.

### Info. Clean-worktree flashes (`kiln_fw_root`)
- **Flash-side archive:** correct. `_repo_root()` comes from `elf_archive.py`'s own `__file__`, so
  the entry lands in the main tree as long as the kilnctrl server runs from the main tree (it does).
  It is not deleted with the worktree.
- **CMake POST_BUILD:** writes `KilnCtrl-latest.elf` into `<worktree>/firmware/KilnFW/elf_archive/`
  and dies with the worktree. That is harmless: it is only a gitignored convenience pointer.
- **Provenance:** see M1.

### Info. Retention and prune
The diff does not touch `_prune`, `MAX_ARCHIVED_ELFS`, `GRACE_PERIOD_HOURS`, `_is_flash_sourced` or
the superseded logic, and the `-latest.elf` exclusions (`elf_archive.py:528`, `:712`) are unchanged.
Semantics are unchanged.

### Info. `_archive_flashed_elf` failure surfacing
`mcp_server_flash.py:416` now returns a WARNING line instead of an empty string. That is fine, and
the flash still is not failed. The change is inert until the kilnctrl server is restarted.
