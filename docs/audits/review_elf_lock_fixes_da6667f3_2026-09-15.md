# Adversarial review: da6667f3 (elf_archive M1-M3, L1-L5)

Date: 2026-09-15. Read-only review of `tools/PcTools/src/kilnctrl/elf_archive.py` and
`tools/PcTools/tests/test_elf_archive.py` at `da6667f3`, against the findings in
`docs/audits/review_elf_migration_fixes_065d51ac_2026-09-15.md` (76e7966b).
Parent commit is `7a801a15`.

**Verdict: NOT closed.** M1, L1-L5 are genuinely closed. M2 is half closed: the
no-steal half is correct, but the mtime-based stale reclaim it adds introduces a
worse failure than the one it fixed. M3 is **not** closed: its own stated goal
("a crash lookup never stalls") is defeated by that same reclaim loop, which on
this machine hangs **indefinitely** instead of the 30 s stall M3 set out to remove.
Weighted against this module's purpose -- it is the only path from a panic to the
matching ELF -- the net effect of M2+M3 as shipped is negative.

## Verification performed

- Clean worktree `C:\wt\rvelf` at `origin/main` = `da6667f3`, main-tree
  `tools/PcTools/.venv` interpreter.
  - `tests/test_elf_archive.py`: **65 passed**.
  - Full `tools/PcTools/tests/`: **2340 passed, 22 failed, 23 skipped** (880 s).
- Clean worktree `C:\wt\rvpar2` at the parent `7a801a15`, with `da6667f3`'s test
  file overlaid onto the parent's `src/`: **59 passed, 6 failed** (table below).
- Standalone empirical test of `_archive_lock` under a stale-mtime-but-live-holder
  lock (script below).
- Own negative test on the production source, restored by hand, empty `git diff`.
- Both worktrees I created were removed. (Note: `C:\wt\rvpar` and `C:\wt\rvneg`
  already existed from other sessions; `rvneg` was deleted by another session
  while I was using it, and one earlier parent-comparison run landed in `rvpar`
  at an unrelated commit `b2e7017f` -- that run was discarded and redone in
  `rvpar2`. Nothing in this review rests on it.)

## Findings

### HIGH-1 (new, introduced by M2): the stale-lock reclaim busy-spins forever when the holder is alive, so a crash lookup can hang indefinitely

`elf_archive.py:759-775`. The staleness branch ends in `continue`, which skips
**both** the deadline check and the `time.sleep(0.05)` below it:

```python
if age is not None and age > _LOCK_STALE_SECONDS:
    print(... "treating as abandoned" ...)
    try: os.remove(lock_path)
    except OSError: pass
    continue                       # <-- deadline never consulted, never sleeps
if time.time() > deadline: ...     # <-- unreachable once age > 10s
```

If the removal does not succeed, the loop is infinite: exclusive-create fails,
age is still stale, removal fails again, `continue`. On **Windows -- this machine
-- that is the ordinary case**, because the live holder still has the lock file
open (`fd` is held for the whole `with` body) and `os.remove` on an open file
raises `PermissionError`, which is swallowed here. The waiter then spins at full
CPU emitting one WARNING line per iteration, forever.

`timeout_s` is not a bound on this function. Proven empirically:

```
lock file created, fd left OPEN, mtime set to now - 15s
_archive_lock(dir, timeout_s=1.0) called on a worker thread
  -> waiter still running after 15s: True
  -> lock file still present: True
```

The consequence is precisely what M3 was written to prevent. `find_kiln_elf_for_build`
passes `lock_timeout_s=3.0` and wraps the call in `try/except Exception`, but
neither helps: this path raises nothing and returns nothing, so the `except` is
never reached and a crash symbolization lookup hangs rather than degrading to
"no match". M3's comment ("a stale lock stalled the lookup for up to 30s.
Wrapped in try/except with a short lock timeout") describes a bound the code
does not implement.

Fix: the staleness branch must respect `deadline` and sleep, and a failed
removal must fall through to the ordinary give-up path rather than `continue`.

### HIGH-2 (new, introduced by M2 and made more likely by M1): the lock's mtime is never refreshed, so a legitimately slow holder is declared dead after 10 s

`elf_archive.py:707-714, 754-760`. The lock file's mtime is set once, at
`os.open(..., O_CREAT|O_EXCL)`, and is never touched again while the holder
works. `age` therefore measures *how long the holder has been working*, not how
long ago it died. Any critical section exceeding `_LOCK_STALE_SECONDS` = 10 s
has its own live lock declared abandoned by the next waiter.

The docstring's justification -- "this module's own critical sections are all
sub-second (a few small JSON files and, at most, one hardlink)" -- is not true of
the region M1 itself just widened. `_archive()` now holds the lock across
(`:1165-1214`):

- `_migrate_legacy_archive_locked()`, which on a name collision runs
  `_sha256_key()` over **both** files in full (`:1032-1036`);
- `adopt_orphaned_kiln_elfs()`, which calls `scan_elf_for_app_descs()` on every
  unregistered ELF -- and that function does `data = f.read()` on the whole file
  (`esp_app_desc.py:207`) and then a linear magic-word scan, for up to
  `MAX_ARCHIVED_ELFS` = 60 multi-MB ELFs;
- `_prune()`'s deletions.

On this repo's OneDrive-backed working tree, reading hundreds of MB of ELF inside
the lock is comfortably over 10 s. The two fixes interact badly: M1 widened the
critical section, and M2 then added a 10 s liveness threshold calibrated for the
narrow one.

Outcome by platform:
- **POSIX:** the removal succeeds, a second writer enters while the first is
  still inside, and both do a manifest read-modify-write -- which is exactly the
  clobber M1 exists to prevent, and the failure mode whose consequence is the
  permanent loss of a flashed entry's provenance.
- **Windows:** HIGH-1's infinite spin instead.

Fix: refresh the lock file's mtime periodically from the holder (or store the
holder's pid and check liveness), and raise the threshold well above the true
worst-case critical section.

### HIGH-3 (new): the lock has no ownership token, so a reclaim makes the previous holder unlock somebody else's lock

`elf_archive.py:786-798`. The M2 fix gates removal on `fd is not None`, which
establishes only that *this call created a lock file at some point* -- not that
the file currently at `lock_path` is still that file. After any reclaim (HIGH-2)
or after two waiters both take the staleness branch, the `finally` block's
`os.remove(lock_path)` deletes whichever lock is there now, which is the new
holder's. That cascades: the new holder is then unprotected, and a third caller
can create the lock and enter. The lock file is written empty; writing the pid
and a uuid into it and comparing before removing would close this, and would also
give HIGH-2 a real liveness signal.

### MED-1: `_archive()` still proceeds *without* the lock on timeout, so M1's guarantee is conditional

`elf_archive.py:776-782`. On the give-up branch the context manager yields with
`fd = None` -- the caller runs the critical section unlocked. `_archive()`'s new
docstring claims "nothing else can observe or mutate this archive's manifest in
between", which holds only when the lock was actually acquired. The clobber M1
fixes is therefore still reachable under contention, just rarer. For a lookup,
degrading to unlocked is defensible (it only reads). For `_archive()`, whose
locked region is a read-modify-write whose failure permanently destroys flash
provenance, proceeding unlocked is the wrong default -- it should fail the
archive call loudly instead.

### MED-2: the new tests are substantially not load-bearing, and two of the four "negative" tests repeat the exact vacuous pattern the previous review flagged

Running `da6667f3`'s test file against the parent's `src/` (`C:\wt\rvpar2`,
59 passed / 6 failed) gives, for the 12 new tests:

| test | vs parent | verdict |
|---|---|---|
| `ArchiveLockTest::test_timeout_waiter_does_not_delete_a_live_holders_lock` | **PASSES** | **not load-bearing** -- this is M2's *core bug* test. It holds `fd` open, so on Windows the old unconditional `os.remove` fails with `PermissionError` and the lock survives anyway. It cannot detect the bug it names on this machine. |
| `ArchiveLockTest::test_negative_reproduces_the_lock_steal_without_the_owner_check` | PASSES | vacuous -- hand-writes `os.remove()` inline and asserts on it; never calls production code. |
| `KilnFwLegacyMigrationLookupTest::test_negative_without_lookup_side_migration...` | PASSES | vacuous -- asserts `_load_manifest()` on an empty dir is `{}`; reconstructs the "old" state by hand. |
| `KilnFwLegacyMigrationLookupTest::test_kilnfw_legacy_manifest_is_merged_and_findable...` | PASSES | expected -- L4 is added coverage of behaviour the parent already had. |
| `ArchiveLockTest::test_owned_lock_is_removed_on_clean_exit` | PASSES | expected (parent behaved the same). |
| `LookupTime...::test_kilnfw_lookup_survives_a_migration_exception` | FAILS (`OSError` propagates) | **genuine** |
| `LookupTime...::test_saftyfw_lookup_survives_a_migration_exception` | FAILS (`OSError` propagates) | **genuine** |
| `ArchiveLockTest::test_contention_serializes_two_holders` | FAILS (`PermissionError` on `.archive.lock`) | genuine, incidentally -- it trips the parent's unconditional-remove bug, though as an error rather than the assertion it advertises. |
| `ArchiveLockTest::test_stale_lock_is_reclaimed_without_waiting_the_full_timeout` | FAILS (`AttributeError: no _LOCK_STALE_SECONDS`) | artifact -- fails on a missing symbol, not behaviour. |
| `LegacyMerge...::test_l2_entry_with_no_elf_anywhere_is_not_merged` | FAILS (`TypeError: takes 2 positional arguments but 3 were given`) | artifact -- `_merge_legacy_manifest` gained a parameter. |
| `LegacyMerge...::test_l3_merged_entries_are_renumbered_past_the_destination_max` | FAILS (same `TypeError`) | artifact. |

So 3 of 12 fail against the parent for a behaviourally meaningful reason, 3 fail
as signature artifacts, and 5 pass. Most importantly, the single test covering
the M2 defect is one of the five that pass. The commit message's own note that a
"Windows `PermissionError` masking a negative test" was fixed during development
applies to a *different* test; this one is still masked by it. The two hand-built
negatives repeat verbatim the criticism the 065d51ac review made of the previous
round ("Each rebuilds the 'old' state by hand ... None of them runs the old
code").

No test covers HIGH-1 or HIGH-2 at all: `test_stale_lock_is_reclaimed...` closes
the fd before `os.utime`, i.e. it tests only the crashed-holder case, which is
the one case that already worked.

### LOW-1: the pre-existing-failure characterisation is 19/22 accurate

Independently reproduced: **22 failed** in a clean worktree at `da6667f3`, and
none touch `elf_archive` (all 65 `test_elf_archive.py` tests pass). But they do
not all have the claimed cause. 19 are the missing untracked
`logs/coupling/*.jsonl` fixtures (`test_coupled_ident.py` x5,
`test_load_estimator.py` x2, `test_pid_ab_compare.py` x12). The other 3 --
`test_logic_capture_paths.py::DefaultOutDirCwdIndependenceTests::test_matches_the_directory_that_already_exists_on_disk`
and both `test_mcp_server_saleae_paths.py::SaleaeCaptureDefaultOutDirTests` tests
-- fail on a different pre-existing cause: they assert an output directory
already exists on disk, which is untrue in any fresh checkout. Pre-existing and
unrelated either way; the conclusion stands, the stated cause does not fully.

### LOW-2: `_archive()` copies the ELF and `<prefix>-latest.elf` outside the lock

`elf_archive.py:1158-1163`. `shutil.copyfile(elf_path, dest)` and the `latest`
copy both run before `with _archive_lock(...)`. Two concurrent flashes can
interleave writes to `latest.elf`, leaving a torn file. Pre-existing, and low
consequence only because CLAUDE.md already treats `latest.elf` as untrustworthy
for symbolization -- but it is the kind of file that gets hand-symbolized against
anyway.

## Checked and OK

- **Manifest atomicity is sound.** `_write_manifest` and `_write_superseded`
  (`:551-557`, `:574-580`) both write `path + ".tmp"` and then `os.replace(tmp, path)`.
  `os.replace` is atomic on Windows (`MoveFileEx` with `REPLACE_EXISTING`), so a
  crash mid-write leaves either the old manifest or the new one, never a
  truncated or missing file. No delete-then-rename anywhere in this module --
  the "Move-Item -Force is not atomic" lesson is respected.
- **No overwrite-by-migration.** `os.link` + `os.remove` throughout; `os.link`
  raises `FileExistsError` and never overwrites on either platform. `shutil.move`
  is not used.
- **M1 is structurally correct.** `_archive()` holds one `_archive_lock` across
  migrate + adopt + load + write + prune (`:1165-1214`), and calls
  `_migrate_legacy_archive_locked()` directly rather than the lock-taking
  wrapper. I checked every `_archive_lock` call site (`:990`, `:1165`) and every
  callee inside the locked region (`_migrate_legacy_archive_locked`,
  `_merge_legacy_manifest`, `adopt_orphaned_kiln_elfs`, `_load_manifest`,
  `_write_manifest`, `_load_superseded`, `_write_superseded`, `_prune`) -- none
  re-acquires it, so the wider hold introduces no deadlock and no lock-ordering
  problem (there is only one lock). Subject to MED-1 and HIGH-2/3, which attack
  the lock's reliability rather than M1's structure.
- **L1 closed.** The byte-identical collision branch (`:1017-1043`) hashes both
  sides and removes the legacy copy only on a proven match, falling back to
  `skipped` if the removal fails.
- **L2 closed.** `_elf_present()` (`:868-873`) gates both merge and
  `merged_keys` membership, and the cleanup pass (`:1088-1104`) re-reads the raw
  JSON and requires every entry's `elf_key` to be in `merged_keys` before
  deleting the legacy manifest -- so an unmergeable entry keeps its file. Proven
  by my own negative test below.
- **L3 closed.** `_renumber()` (`:882-887`) assigns from `1 + max(seq)` over both
  the destination manifest and superseded buckets, iterating legacy entries in
  legacy-`seq` order.
- **L5 closed.** `migrate_legacy_provenance` (`:359-378`) compares mtimes and
  warns when the ignored legacy record is the newer one, without auto-merging.
- **Provenance preservation is intact.** Merged entries keep their original
  `source`/`archived_at`; a lost identity collision goes to `superseded.json`,
  which `_prune` still protects via `_is_flash_sourced`.

## Own negative test

Performed in `C:\wt\rvelf` (clean worktree at `da6667f3`), production source
poisoned by hand:

```
baseline: git diff on elf_archive.py -> empty
poison:   `if age is not None and age > _LOCK_STALE_SECONDS:`
       -> `if False and age is not None and age > _LOCK_STALE_SECONDS:`
run:      1 failed, 64 passed
          FAILED ArchiveLockTest::test_stale_lock_is_reclaimed_without_waiting_the_full_timeout
restore:  inverse text substitution applied BY HAND (no git checkout/restore/stash)
after:    git diff on elf_archive.py -> empty; git status --porcelain -- tools/ -> clean
```

The suite does detect removal of the stale-reclaim branch. It does not detect
either of the defects that branch introduces.

## Recommended order of fixes

1. HIGH-1 -- the staleness branch must honour `deadline` and sleep; a failed
   removal must fall through to the give-up path. This one can hang a crash
   lookup today.
2. HIGH-3 -- write pid+uuid into the lock file and verify before removing.
3. HIGH-2 -- refresh the mtime from the holder, or drop the mtime heuristic in
   favour of the pid check from HIGH-3, and raise the threshold.
4. MED-1 -- make `_archive()` fail loudly rather than run unlocked.
5. MED-2 -- rewrite `test_timeout_waiter_does_not_delete_a_live_holders_lock` so
   it is not masked by the Windows open-handle accident (hold the "live" lock
   from a subprocess, or assert against a monkeypatched old implementation of
   the real function), and replace the two hand-built negatives with ones that
   run production code.
