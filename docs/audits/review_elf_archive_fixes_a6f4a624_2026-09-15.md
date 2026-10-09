# Adversarial review: commit a6f4a624 (elf_archive move review fixes)

Date: 2026-09-15. Scope: fixes for M1, M2, L1, L3 and L4 from
`docs/audits/review_elf_archive_move_a347e726_2026-09-15.md`. Read-only review;
no board, flash or MCP server was touched.

## Completeness check

- a6f4a624 is on origin/main. A clean detached worktree at origin/main
  (`C:\wt\rv_a6f4`) ran `test_elf_archive.py`, `test_flash_board_pinning.py`,
  `test_flash_firmware_verify.py` and every other test file mentioning
  provenance: **134 passed**. The worktree has been removed.
  (`uv run --frozen` failed there because numpy was missing, so the run used the
  main tree's `.venv` interpreter with `PYTHONPATH=src`.)
- Negative test: I disabled the `os.path.exists(dst)` skip in
  `migrate_legacy_archive()`. `test_never_overwrites_an_existing_destination_name`
  then FAILED, because on Windows `shutil.move` falls back to copy+unlink and
  overwrites. Restored by copying back a saved copy of the file (not with git);
  `git status --porcelain` came back empty.

## Findings (most severe first)

### M1 -- The migration strips flash provenance, so migrated flashed ELFs become deletable
`tools/PcTools/src/kilnctrl/elf_archive.py:697` (the skip on name collision),
together with `:744` (runs before adoption), `:610` (`archived_at` comes from
mtime), and `_prune` at `:871`/`:883`.

Failure scenario: the old `build/elf_archive/` holds flashed `KilnCtrl-<key>.elf`
files and a `manifest.json` whose entries say `source: flash_firmware...`. The
new directory already has a `manifest.json`, which any flash since a347e726
creates. The migration:
1. moves the ELF files;
2. leaves the old manifest stranded, because the name collides (it is never merged);
3. `adopt_orphaned_kiln_elfs()` then re-registers the moved ELFs as
   `adopted:orphan-scan`, with `archived_at` taken from the file's mtime
   (`shutil.move` keeps mtime, so the date is weeks old).

Result: images that were really flashed lose `_is_flash_sourced` protection
and are already past the 48 h grace window. The next `_prune` with more than
`MAX_ARCHIVED_ELFS` files deletes them oldest-first, which is exactly the
"older flashed build can still be the running one (OTA/otadata)" case the
retention policy exists to protect. SaftyFW is worse: it has no adoption path,
so moved `SaftyFW-*.elf` files are unregistered and cannot be found by
`find_safty_elf_for_identity` (their manifest is left behind), and they are
eligible for pruning by mtime (`:878-883`). The docstring's claims that entries
are "recovered on a best-effort basis" and that it will "never delete unmoved
data" are true file by file, but false for the provenance that makes the data
safe to keep.

Fix: merge the legacy manifest and `superseded.json` entries into the new ones
(keep the existing entry on an identity clash, and push the legacy entry into
superseded) **before** moving any ELFs. Only remove the legacy manifest after
the merged manifest is written.

The bench today is not exposed: the legacy dir holds an empty `{}` manifest
and a 32-byte test-debris `KilnCtrl-latest.elf`. Any other clone or worktree
with real flashed entries under `build/elf_archive/` would be.

### M2 -- The migration runs only inside `_archive()`, so lookups cannot see legacy entries until a flash
`elf_archive.py:744`. `find_kiln_elf_for_build` (`:982`),
`find_safty_elf_for_identity` (`:1027`), `find_crash_elf`
(`mcp_server_flash.py:939`) and the Pico counterpart never call it.

Failure scenario: the board panics while running an image archived under the
old layout. `find_crash_elf()` reports "no archived ELF" and gives no hint
that the ELF is one directory away. Following the tooling, the operator ends
up symbolizing against `build/KilnCtrl.elf`, which is the wrong-ELF mistake
CLAUDE.md warns about. The next step taken to "fix" things is usually a
reflash, which then runs the migration (and, per M1, may prune the
now-unprotected image).

Fix: on a miss, have the lookups also consult the legacy directory read-only,
or at least name it in the miss message.

### L1 -- The legacy dir never empties, so every flash prints a warning forever
`elf_archive.py:688-690` and `:697-705`. The skipped `<prefix>-latest.elf` and
any colliding `manifest.json` stay put, so the "fast no-op once drained" path
(`:684`) is never reached. On the bench today, every KilnFW flash will print
"left 1 legacy entry(ies) ... unmoved: manifest.json". A warning that fires
on every flash gets ignored, which also hides a real M1 collision.

Fix: delete the stale latest pointer and an empty `{}` manifest, since neither
holds data. Then remove the directory once it is empty.

### L2 -- Two concurrent archives can lose the migrated manifest
`elf_archive.py:744` versus `_load_manifest`/`_write_manifest` (`:760`, `:790`).

Failure scenario: process A (`flash_firmware`) loads the new manifest before B
exists, or as absent. Meanwhile process B's `_archive` migrates the legacy
`manifest.json` into place (there was no collision yet). A then writes its
own manifest and overwrites the migrated one, so the legacy entries are
silently gone. There is also a check-then-move TOCTOU at `:697`/`:707`: on
Windows `os.rename` refuses an existing dst, but `shutil.move`'s copy+unlink
fallback overwrites it. Each individual move is atomic (same-volume rename),
and a failure mid-loop leaves a consistent split: every file sits wholly in
one directory or the other. The manifest race already existed before this
commit, but this commit adds a new way to lose data through it. Low
likelihood, because sessions rarely flash the same board at the same moment.

Fix: take a lock file around migrate+manifest, or use the merge from M1.

### L3 -- Existing `build/flash_provenance.json` is not migrated, and a stale server keeps writing the old path
`mcp_server_info.py:338` now reads only `elf_archive.kiln_provenance_path()`.

Failure scenario: the last recorded outcome was `refused_sensitive_dirty` /
`flash_failed` in `build/flash_provenance.json`. After this commit,
`get_fw_version`'s last-flash warning quietly disappears, because the file is
not found, and `read_provenance_json` returns None with no warning. Also, a
kilnctrl server started before a6f4a624 (see CLAUDE.md "Stale-server
self-announcing") keeps writing to `build/` while a fresh reader looks at
`firmware/KilnFW/`. This is transitional; a server restart and one flash
clear it.

Fix: fall back to reading the legacy path when the new one is absent.

### L4 -- Tests: the L4 pin cannot catch a regression, and the provenance reader and guard are untested
- `CanonicalArchiveDirsMatchTest` (`tests/test_elf_archive.py`) passes on the
  pre-fix code too, since both copies of the path were already equal. It pins
  against future drift, not the a347e726 bug. That is fine, but the commit
  message says every new test was negative-tested against production code,
  which cannot be true of a pin like this in the "would have failed before"
  sense.
- No test covers `_guard_against_test_write()` refusing
  `_canonical_provenance_path()`, or `get_fw_version` reading the new path.
  Reverting `mcp_server_info.py:338` to `build/flash_provenance.json` would
  pass the whole suite.
- None of the migration tests cover M1's collision-plus-adoption interaction
  or SaftyFW.

The rewritten tests that do fail against the old code: the L3 build-wipe test
(the old nested `kiln_archive_dir()` gets rmtree'd), the
`test_provenance_json_records_*` pair (the old code wrote under `build_dir`,
so reading from `prov_path` returns None), `test_wired_into_archive_kiln_elf_automatically`
(there was no migrate call), and the overwrite test (confirmed above).

### Info -- Readers and writers of flash_provenance.json, and SaftyFW parity
- Writers: only `flash_firmware()` (`mcp_server_flash.py:633/641/654/718/729/738`),
  all via `provenance_path = elf_archive.kiln_provenance_path()` (`:617`), with
  the test guard at `:618`. The sensitive-dirty guard (`:626`) writes there on
  refusal, which is correct.
- Readers: only `get_fw_version` (`mcp_server_info.py:338`).
  `capability_preflight.py` and `stale_check.py` (the /health staleness check)
  never read or write it; grep finds no reference, so nothing was missed. No
  `.ps1`/cmake script references it.
- `.gitignore`: `firmware/KilnFW/.gitignore:31-36` covers the new path.
- SaftyFW parity: `debug_program(peer="pico")` (`mcp_server_debug.py:191`)
  archives the ELF but records **no** flash provenance at all: no tree
  capture, no sensitive-dirty guard, no persisted record. This gap existed
  before the commit and is not a regression, but the claim that "provenance
  moved for both firmwares" applies only to KilnFW. Worth a roadmap item,
  since SaftyFW safety code is the most sensitive dirty-tree case.
- Docs (M2): CLAUDE.md, MCP_SERVERS.md and BRINGUP_HAZARDS.md are updated.
  `docs/FILESYSTEM.md:833,979` still name `build/elf_archive/`, but as
  historical records, which is acceptable.
