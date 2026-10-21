# Review: release ELF archive (dbbf5b735), 2026-10-20

Scope: `tools/archive_release_elf.py`, the `make_release.ps1` hook, the ELF MISSING / NOT SYMBOLIZED reporting in `diag_bundle.py` and `mcp_server_flash.py`, and `tests/test_diag_bundle.py`. Review only. make_release was not run. `test_diag_bundle.py`: 46 passed.

Summary: HIGH 1, MED 1, LOW 4, INFO 4. Findings use the prefix RA-.

## Identity check (passes)
The firmware's `dump_elf_sha` comes from `esp_core_dump_get_summary().app_elf_sha256`. This is the hex prefix of the SHA256 of the linked ELF file, 9 chars by IDF default (`CRASH_REPORT_ELF_SHA_MAX` 19 allows up to 18). The archive file name is `KilnCtrl-<sha256(elf)[:12]>.elf` (`_sha256_key`). `find_elf_by_sha_prefix` compares the dump prefix against that key. Both sides hash the same file, so a release or OTA image's dump resolves to the archived ELF as long as the file is present (see RA-01). The 9-char prefix fits inside the 12-char key. If anyone raises CONFIG_APP_RETRIEVE_LEN_ELF_SHA above 12, the `len(key) >= len(p)` check will report every dump as ELF MISSING (RA-07).

## HIGH
- **RA-01** The archive is written to the worktree's own gitignored `elf_archive/`, not the main tree's. `_repo_root()` is resolved relative to the file location (`elf_archive.py`, four levels up), even though the `kiln_archive_dir()` docstring says "always the MAIN tree's". `make_release.ps1` requires a clean tree and tells the user to build "from a clean worktree (tools\worktree_mint.ps1)". The release ELF therefore lands in `C:\wt\<rel>\firmware\KilnFW\elf_archive`, and the routine `git worktree remove --force` deletes it. `diag_bundle_fetch` then runs from the main-tree MCP server and reports ELF MISSING for the shipped image. The goal of the commit fails in the documented release flow. The only durable copy left is the `KilnCtrl-<tag>.elf.zip` release asset. Fix: resolve the main tree through `git rev-parse --git-common-dir` in `_repo_root()` or in `archive_release_elf.py`, or pass the main tree's archive dir explicitly. Then check that the file exists there.

## MED
- **RA-02** The recovery image ELF is not archived. make_release ships `recovery.bin` (`$RecoveryBin`), and `find_elf_by_sha_prefix` searches `list_recovery_elf_paths()`, but nothing archives the recovery ELF that a release ships. A coredump from the shipped recovery image reports ELF MISSING. Fix: archive `recovery.elf` beside it, and fail the release if that step fails.

## LOW
- **RA-03** Without -Publish, a non-dry run that uses `-SkipBuild` or `-BuildDir` archives a build that was not made here, stamped with HEAD's commit and `make_release:<tag>`. The content-hash key is still right, but the recorded provenance can be wrong. Fix: skip archiving, or record "external build", whenever -SkipBuild or -BuildDir is set.
- **RA-04** The tests only cover the failure exit codes (1 for missing inputs, 2 for bad argc). No positive test archives into a monkeypatched tmp archive dir and asserts the key equals `sha256(elf)[:12]` with an `elf_key` manifest entry. No test pins the make_release wiring: the call sits under `-not $DevDryRun`, and `Fail` runs on a non-zero exit. No negtest is recorded for the new tests. -DevDryRun skips archiving entirely, so a dry run never exercises this path.
- **RA-05** The ELF MISSING text says to restore the ELF to `firmware/KilnFW/elf_archive`. It should name the source: the `KilnCtrl-<tag>.elf.zip` GitHub release asset.
- **RA-06** The ROADMAP FD "ELF archive" box was ticked while RA-01 and RA-02 are open.

## INFO
- **RA-07** The key/prefix length coupling described above (12-char key, IDF 9-char prefix) is undocumented. Pin it with a test or a comment.
- **RA-08** Overwrite and delete safety is fine. `_archive` holds `_archive_lock`, copies to a content-hash name (a re-copy writes identical bytes), and only rewrites `KilnCtrl-latest.elf` and the manifest. It deletes no hash-keyed ELF. A same-`fw_build` / different-content collision goes to the superseded registry.
- **RA-09** Failure handling is clean. A non-zero exit from the script triggers `Fail` before the manifest and publish steps. The archive happens before publish, so a later publish failure leaves an extra archived ELF, which is harmless.
- **RA-10** OTA-only images: the content-hash match works for any image whose ELF was archived. Dev builds rely on the `archive_elf.cmake` POST_BUILD copy plus orphan adoption, which writes into the building tree and has the same worktree durability gap as RA-01. When the sha lookup succeeds, the symbolizer still gets every candidate ELF instead of the matched one. That is correct, just slower.
