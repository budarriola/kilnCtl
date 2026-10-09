# Filesystem-migration review, 2026-09-07

Whole-of-track review of the `cfg` LittleFS work that landed today, before
more is built on it. Commits reviewed: `ca5d90c5`/`66bd57e5` (LittleFS
component pinned), `234ce9f3` (partition table, `cfg` at 0xDB0000),
`c4b4e65d` (that table actually flashed to the bench board), `d2a1358d`
(`cfg_fs` foundation), `b79b5ef5` (`/api/cfgfs` + `get_cfgfs_status`),
`19f74959` (zones config dual-write), plus the prefs-move and profiles-move
work in the working tree at review time. Plans read: `docs/FILESYSTEM_PLAN.md`,
`docs/FILESYSTEM_USER_DATA_PLAN.md`.

Everything below is from the code, not the commit messages. Where a claim in
a plan or commit message did not survive checking, that is said explicitly.

---

## Summary

| Question | Verdict |
|---|---|
| 1. Atomicity real on-device? | **Yes**, verified against the pinned component's source, not inferred. One caveat about who calls it. |
| 2. Dual-write consistent? | **No — one real defect, fixed here.** The equal-rev tie-break adopted the OLDER copy in exactly the rollback case the rev counter exists for. |
| 3. Partition-absent identical? | **Verified, with two named, benign deviations** — the claim is not literally byte-identical. |
| 4. Rollback / NVS unconditional? | **Safe on every migrated item checked.** No item gates its NVS write on file success. |
| 5. Test honesty | **Good.** Real production functions, real filesystem, negative tests broke production code. One coverage gap (now closed). |
| 6. Boot safety | **Cannot brick — because nothing is wired.** `cfg_fs_mount_device()` has zero callers, and there is no path that could ever format the partition. |

New check added: `firmware/KilnFW/App/test/check_cfg_fs_tie_break.ps1`
(negative-tested against production code; see section 7).

---

## 1. Is the atomicity claim real on-device?

**Yes.** Traced through `cfg_fs_write_atomic()`
(`firmware/KilnFW/App/drivers/persist/cfg_fs.c`) and down into the pinned
component at `firmware/KilnFW/managed_components/joltwallet__littlefs`
(1.22.3, hash-pinned in `dependencies.lock`), not assumed from the host fake.

The sequence is `fopen("wb")` → `fwrite` → `fflush` → `cfg_fs_fsync` →
`fclose` → `cfg_fs_atomic_rename`, with every failure branch removing the
temp file and returning without touching the final path.

- **`fflush()` alone would not be enough** and the code knows it — it calls
  `fsync(fileno(f))` on the POSIX/newlib path. That matters, so it was
  checked that the VFS actually implements it rather than returning
  `ENOSYS`: `esp_littlefs.c` registers `.fsync_p = &vfs_littlefs_fsync`
  (line 445, and again in the ≥IDF-5.4 table at line 769), and
  `vfs_littlefs_fsync()` (line 2238) calls `esp_littlefs_file_sync()` →
  `lfs_file_sync()`, which flushes littlefs's own cache and commits the
  file's metadata to flash. This is a genuine flush-to-flash, not a libc
  buffer flush. (`fsync_p` is NULLed only on a read-only mount — `cfg` is
  not mounted read-only.)
- **The rename is atomic.** `vfs_littlefs_rename()` (line 2405) delegates to
  `lfs_rename()`, which performs a single metadata-pair commit; littlefs's
  metadata pairs are CRC'd and alternately written, so the directory entry
  either points at the old file or the new one, never at a half state.
  No directory-fsync step is needed or possible — littlefs has no
  write-back page cache for metadata the way a POSIX kernel does; the
  commit *is* the durable write.
- One on-device behaviour that is not modelled by the host fake but is
  benign here: `vfs_littlefs_rename()` returns `EBUSY` if either source or
  destination is currently open. `cfg_fs_write_atomic()` `fclose()`s the
  temp file before renaming and never holds the final path open, so it
  cannot hit this. Worth remembering if a future caller ever renames while
  reading.
- The `.tmp/` sweep (`sweep_tmp()`, called from `cfg_fs_init()`) only ever
  removes regular files directly inside `<base>/.tmp/` and never touches a
  final path, so a sweep can never destroy committed data.

**So the guarantee holds on-device, not only on the host fake.** The host
fake earns its keep for a different reason: `cfg_fs.c` is compiled and run
for real in the host tests (`_commit()`/`MoveFileExA(...,
MOVEFILE_REPLACE_EXISTING)` on Windows), so the *control flow* is exercised
even though the LittleFS semantics come from the component above.

**Caveat that is about callers, not about the helper.** The plan makes it
mandatory that "every config-FS write goes through the flash worker"
(`project_psram_stack_nvs_panic`). `cfg_fs_mount.c` provides exactly that
wrapper — `cfg_fs_write_atomic_device()`, which dispatches through
`uart_bridge_ext_run_on_flash_worker()`. But **every bridge module defaults
its write function to the bare `cfg_fs_write_atomic`**, and nothing in
production boot code ever calls `zones_config_cfg_fs_set_write_fn()` (nor
its prefs/profiles equivalents) to install the device wrapper. Today that is
inert (see section 6 — nothing is mounted), but it is a loaded gun: the
commit that finally wires `cfg_fs_mount_device()` into boot will, unless it
also remembers three separate setter calls, start doing flash writes from
whatever task called `save()`. This is exactly the "reset one side of a
pair" shape — the mount decision and the writer-installation decision are
two pieces of state joined only by a comment. **Recommendation: install the
device write function from inside `cfg_fs_mount_device()` itself**, so the
mount and the writer cannot be wired independently, rather than leaving it
to a caller to remember.

## 2. Is the dual-write actually consistent? — ONE REAL DEFECT, FIXED

The three bridge modules (`zones_config_cfg_fs.c`, `pref_cfg_fs.c`,
`profiles_cfg_fs.c`) all implement the *same* resolve/tie-break rule rather
than each inventing one — that part is good, and is the thing this repo
most often gets wrong. The rule they shared, however, was wrong in one
interleaving.

### The write order and the rev

`zones_config_store.c`'s `nvs_save()`:

```
s_zones_cfg_rev++;
(void)zones_config_cfg_fs_save(&s_zones.cfg, s_zones_cfg_rev);   /* FILE first */
hal_kv_set_blob(&h, NVS_KEY_ZONES, ...);                          /* then NVS blob */
hal_kv_set_u32(&h, NVS_KEY_ZONES_REV, s_zones_cfg_rev);           /* then NVS rev */
```

Both sides are stamped with the *same* new rev. Enumerating every way the
two can end up disagreeing:

| Interleaving | file_rev | nvs_rev | Who is newer | Old `>=` rule | Correct |
|---|---|---|---|---|---|
| Crash after file write, before NVS | N+1 | N | file | file ✔ | file |
| File write fails, NVS lands | N | N+1 | NVS | NVS ✔ | NVS |
| Crash between `set_blob` and `set_u32` | N+1 | N | file | file ✔ | file |
| **Edit made on rolled-back firmware** | **N** | **N** | **NVS** | **file ✘** | **NVS** |

The last row is the defect. Firmware rolled back past this change still
writes `NVS_KEY_ZONES` but knows nothing about `zones_rev`, so it leaves the
rev where it was. Rolling forward then finds **equal revs with differing
bytes** — and `if (file_rev >= nvs_rev)` adopted the **stale file**,
discarding the rolled-back-firmware edit. Worse, it is not merely a bad
read: `nvs_load()` adopts the file, sets `s_zones_cfg_rev` to it, and the
next `nvs_save()` overwrites the NVS copy, so the lost edit is gone
permanently.

This is precisely the hazard `FILESYSTEM_USER_DATA_PLAN.md` section 4
introduced the rev counter to close ("an edit made on *rolled-back*
firmware writes NVS only; rolling forward again then reads the *file*, which
is now stale, and silently loses that edit"). The implementation had the
counter and then resolved the tie in the direction that defeats it.

Note that **equal revs with differing bytes can never legitimately mean
"the file is newer"** — both sides always stamp the same new rev, so the
file can only be ahead by a strictly higher one. That makes NVS-on-equal
unconditionally correct, not a heuristic. The second producer of the same
state (a crash between `hal_kv_set_blob()` and `hal_kv_set_u32()`) also puts
NVS ahead at an equal rev, so it is fixed by the same change.

**Fixed** in `zones_config_cfg_fs.c`: `file_rev >= nvs_rev` →
`file_rev > nvs_rev`, with the enumeration above written into the branch
comment and into `zones_config_cfg_fs.h`'s tie-break paragraph.

**`profiles_cfg_fs.c` was fixed by its own (concurrent) pass during this
review.** **`pref_cfg_fs.c` still carries the defect at review time** and is
owned by the prefs-move pass — it is out of this review's edit scope, is
named in the new check's grace list, and needs the identical one-character
change.

### Everything else in the resolve path checks out

- Content is compared (`memcmp`) before rev, so two independently-identical
  copies never log a spurious divergence and never fight over rev.
- The losing side is resynced from the winner, so a divergence does not
  survive to the next boot (the file directly, NVS via the caller's next
  save).
- `zones_config_cfg_fs_save()` re-stamps `crc32` on its own local copy
  before writing rather than trusting the caller's — a genuinely good catch
  by that pass, since `nvs_load_from()` mutates the struct *after* CRC
  validation via `zones_config_json_normalize_settings_source_cycles()`. A
  file written from such a struct would have failed its own next decode.
- Rev-counter initialisation is safe: `zones_http.c:612` calls `nvs_load()`
  (which seeds `s_zones_cfg_rev` from `zones_rev`) before
  `migrate_from_default_partition()` at line 634, the only other `nvs_save()`
  caller at boot. A `save()` before a `load()` would have started from rev 0
  and lost to any existing file — that ordering is load-bearing and is now
  worth not disturbing.

### One inconsistency between modules worth noting (not a defect today)

`zones_config_store.c` advances its in-RAM rev unconditionally
(`s_zones_cfg_rev++` before either write), while `ramp_assist_cfg.c` only
adopts `new_rev` on NVS success. Both are self-consistent — the ramp-assist
one simply re-uses the same rev on a retry, overwriting its own file — but
the two conventions differ, and a third module copying the wrong one while
assuming the other's invariant is how this class starts. Worth unifying
before more items move.

## 3. Does the partition-absent path really behave identically?

**Verified rather than trusted, and the "byte-identical" claim is very
nearly but not literally true.** Two deviations, both benign, both worth
stating because the claim was made strongly:

1. **An extra NVS read per load.** `nvs_load()` now calls
   `zones_cfg_rev_load()` unconditionally, which opens the namespace and
   reads `zones_rev`, on every board including partition-absent ones.
2. **An extra NVS write per save, and it can change the return value.**
   `nvs_save()` now writes `NVS_KEY_ZONES_REV` after the blob and folds its
   status into the returned error. A failure of that second `set_u32` — a
   full NVS page, say — turns a previously-successful save into a reported
   failure. The blob itself still landed. This is a new, if unlikely, way
   for `nvs_save()` to report failure that did not exist before.

The file layer itself is genuinely inert: `cfg_fs_is_available()` is checked
first in `cfg_fs_read/_write_atomic/_exists/_delete/_list` and in every
bridge entry point, so every file operation is a fast no-op returning
`ESP_ERR_INVALID_STATE`, which the callers deliberately swallow. The control
flow through `zones_config_cfg_fs_resolve()` with `file_valid == false`
returns the NVS candidate unchanged with `used_file == false`, and
`trustworthy` then equals the old `out_valid`, so
`zones_config_push_all_relay_types()` is gated exactly as before.

Host-tested directly by `test_zones_config_cfg_fs.c ::
test_partition_absent_falls_through_to_nvs_only`, which additionally asserts
no file was created.

**Correction to a standing claim in both plan documents:** they repeatedly
say "no `cfg` partition on any board today". That stopped being true at
`c4b4e65d` — the bench board was flashed with the new table on 2026-09-07
and *does* have `cfg` at 0xDB0000. The board is still in the partition-
*unformatted* state, which behaves the same way (mount fails, see section 6),
but the plans' wording should be corrected so a future reader does not
conclude the partition is absent when it is merely empty.

## 4. Rollback: are the NVS writes genuinely unconditional?

**Yes, on every migrated item inspected.** Checked one at a time, looking
specifically for an NVS write gated on file success:

| Item | Where | NVS write unconditional? |
|---|---|---|
| Zones config (1) | `zones_config_store.c :: nvs_save()` | **Yes** — the file save is `(void)`-cast and its result is never branched on; the `hal_kv_open`/`set_blob`/`commit` sequence follows unconditionally and is what the return value reports. |
| Ramp assist (10) | `ramp_assist_cfg.c :: ramp_assist_cfg_set_enabled()` | **Yes** — `pref_cfg_fs_save()`'s error is logged and dropped; NVS proceeds regardless. |
| Unit pref (11), display power (12) | same pattern, `unit_pref.c` / `display_power_cfg.c` | **Yes** — same file-first-then-unconditional-NVS shape. |
| Profiles (5,6) | `profiles_http.c :: nvs_save_slot()` | **Yes** — `(void)profiles_cfg_fs_save(...)`, then the NVS blob and the `prof_rev` array unconditionally. |

No item strands data on a file-only write. Combined with the section 2 fix,
the rollback round trip is now safe in *both* directions: an edit made on
new firmware reaches NVS for the old build to read, and an edit made on the
old build survives the roll-forward instead of being discarded.

The pre-existing `ota_rollback_esp()` / `zones_cfg` schema-bump hazard
documented in CLAUDE.md is unchanged by this work — not widened, not fixed.

## 5. Test honesty

Checked for the three failure modes this repo has actually shipped: mirrors
standing in for production code, negative tests that break a test-local
copy, and assertions derived from the code under test.

- **Real production functions, not mirrors.** `test_zones_config_cfg_fs.c`
  links `zones_config_cfg_fs.c` and `cfg_fs.c` as ordinary translation units
  and drives them through the *real* `nvs_load()`/`nvs_save()` (defined in
  `zones_config_store.c`, reached across the link, not re-`#include`d).
  `cfg_fs.c` runs against a real temp directory with real `fopen`/`fsync`/
  `rename` — the only substitution is the fake KV, which is the
  long-established harness.
- **Negative tests broke production code.** Per `d2a1358d`'s own record, the
  `cfg_fs` negative test redirected `cfg_fs_write_atomic()`'s `fopen()` from
  the temp path to the final path — a change to the production function, not
  a copy. Per `19f74959`'s, the dual-write negative test commented out
  `nvs_save()`'s real `hal_kv_set_blob()` call and produced 20+ failures.
  Both are the standard this repo asks for.
- **The one seam that is test-injected is honestly named.**
  `zones_config_cfg_fs_set_write_fn()` exists so a test can simulate a file
  write failure. It is a real production seam (it is how the device wrapper
  is meant to be installed), and the tests that use it still exercise the
  real resolve/tie-break logic around it.
- **No self-referential assertions found.** The strongest test —
  `test_file_migration_matches_direct_blob_decode_v21` — is a genuine vector
  comparison: it stages a v21 blob, decodes it once through the file path
  and once through `zones_config_json_decode_blob()` directly, and asserts
  byte equality. Both sides call the same function, which is the *point*
  (proving there is one migration chain, not two), and it is not the shape
  `check_mcp_tool_count_doc.ps1` was faulted for, because the expected value
  is a hand-built fixture rather than something computed by the code under
  test.
- **Coverage gap, now closed.** The tie-break test exercised only
  `nvs_rev > file_rev` and the agreeing case. The `file_rev == nvs_rev` case
  — the one that was wrong — was never tested. Added
  `test_equal_rev_divergence_adopts_nvs_not_the_stale_file()`, which stages
  the rollback scenario through the real production write paths (a bare
  `hal_kv_set_blob()` on `NVS_KEY_ZONES` with `zones_rev` untouched, exactly
  as pre-dual-write firmware would), asserts the fixture really is the
  equal-rev case before asserting the outcome, and checks the file is
  resynced. **Negative-tested**: reverting the production comparison to
  `>=` makes it go red on both assertions; restored by hand and the full
  suite is green again (29/29 executables, 1550/1550 checks).

## 6. Boot safety

**The board cannot be bricked by this work, for a stronger reason than the
mount-failure contract: none of it runs at boot.**

- `cfg_fs_mount_device()` has **zero callers** anywhere in the tree. Nothing
  in `main_boot_early.c` or the bring-up path references it. So the ordering
  question ("does anything added run before `boot_guard` decides recovery
  mode?") has the trivially safe answer: nothing added runs at all.
- When it *is* wired, the gate is correct and is host-tested as a real
  function: `cfg_fs_mount_or_skip(recovery_mode, ...)` returns `ESP_OK`
  without touching the filesystem when recovery mode is set, and
  `cfg_fs_mount_device()` reads `boot_guard_is_recovery_mode()` before
  anything else. Mount failure returns an error and is documented as
  non-fatal; it does not abort, assert, or retry.
- `format_if_mount_failed` is **false**, correctly — the plan explicitly
  flagged a copy-paste of `log_store_mount.c`'s `true` as the thing to catch
  in review. It was not copied.

**The consequence nobody has written down yet:** the bench board now has a
`cfg` partition that has never been formatted. A blank LittleFS partition
fails `esp_vfs_littlefs_register()` with `format_if_mount_failed = false`,
and **there is no `esp_littlefs_format()` call anywhere in the tree**. So
even once `cfg_fs_mount_device()` is wired in, `cfg_fs` will never become
available on this board — every bridge stays permanently in the
partition-absent no-op path, silently, with the feature looking implemented.
A deliberate, operator-initiated first-time format action is a missing
prerequisite for step 1 to actually work on hardware. It should be explicit
and operator-driven (the plan's factory-reset-page framing is right), not an
automatic fallback — but it has to exist.

## 7. The narrow check added

`firmware/KilnFW/App/test/check_cfg_fs_tie_break.ps1` (auto-discovered by
`tools/run_all_checks.ps1`). It scans
`App/drivers/persist/*_cfg_fs.c` and fails on `file_rev >= nvs_rev` (or the
mirrored `nvs_rev <= file_rev`) in the winner-selection branch, after
stripping comments so the prose explaining why `>=` is wrong does not trip
it.

Why this is narrow enough to be honest by CLAUDE.md's standard: it is pinned
to one concrete, named pair of variables in one directory, with exactly one
correct comparison derived from the enumeration in section 2 — not to the
general "reset one side of a pair" shape, which that document explicitly
rejects as un-checkable.

Three properties that keep it from going vacuous:

1. **Positive presence assertion** — zero bridge modules found is a failure,
   not a pass, so a rename or a file split cannot leave it green with no
   coverage.
2. **Untracked files warn, tracked files fail** — it cannot turn a shared
   tree red under another session's half-finished file, and starts enforcing
   the moment that file is committed.
3. **Self-retiring grace list** — `pref_cfg_fs.c` is exempted (it is owned
   by the concurrent prefs-move pass and out of this review's edit scope),
   but a grace-listed file that *no longer* matches is itself a FAILURE
   demanding the entry be deleted. This fired for real during the review:
   the profiles pass fixed its copy, the check went red demanding the stale
   exemption be removed, and it was.

**Negative-tested against production code**, not a copy: reverting
`zones_config_cfg_fs.c`'s comparison to `>=` produced
`FAIL: zones_config_cfg_fs.c: line 169 ... compares the dual-write revs with
'>='`, exit 1. Restored by hand (`sed` on that one line, `git diff` confirms
the only remaining change to that branch is `>=` → `>`), check green again.

---

## Open items for whoever continues this track

1. **`pref_cfg_fs.c` still has the equal-rev tie-break defect** (section 2).
   One-character fix, plus the equal-rev test case; the check's grace-list
   entry must be deleted in the same commit.
2. **Wire the flash-worker write function to the mount, not to callers**
   (section 1) before `cfg_fs_mount_device()` is called from boot.
3. **There is no first-time format path** (section 6) — without one the
   feature is permanently inert on the already-flashed board.
4. **Correct both plan documents' "no board has a `cfg` partition" wording**
   (section 3); it has been false since `c4b4e65d`.
5. **`/api/cfgfs`'s handler is stack-heavy for the httpd worker.**
   `cfgfs_status_get_handler()` puts a `zones_cfg_t` (~640 B) and a
   `char json[2048]` on the frame and calls
   `zones_config_cfg_fs_load_raw()`, which adds its own 1028-byte buffer —
   roughly 3.7 KB in one call chain, on an 8192-byte httpd worker stack that
   `project_httpd_stack_near_overflow` measured at **64 bytes of margin**
   under real load. Not necessarily the new worst case (handlers do not
   nest), but this must be measured via `stack_margin` before the board is
   flashed with it, not reasoned about.
6. **Unify the in-RAM rev advance convention** across modules (section 2).
