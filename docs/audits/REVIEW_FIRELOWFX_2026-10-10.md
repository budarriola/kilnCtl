# Review: REVIEW_FIRE2 LOW-1/2/3 + INFO-5 fixes (2026-10-10)

Scope: origin/dev `b76d03657` (code + tests) and `21a6add32` (doc), the fixes
for `docs/audits/REVIEW_FIRE2_2026-10-10.md` LOW-1, LOW-2, LOW-3 and INFO-5.
Reviewed at detached `21a6add32` in a clean worktree. Adversarial review; no
code changed.

Verdict: no HIGH or MED. LOW-1 and INFO-5 are sound. LOW-2 has two LOW
findings: the rebuild can delete a live profile, and the new longer-blob shape
accepts junk. LOW-3 has one LOW finding: favorites are still silently reset on
the next write.

## Tests run

- `build_host_tests.ps1 -Only "test_profiles_http\.c|test_profile_executor_prestart\.c"`:
  2/2 executables built and passed.
- `tools\negtest.ps1` over the same two host tests, with four mutations:

  baseline passed, all four mutations CAUGHT, real tree unchanged.
  - LOW-2 rebuild never runs (`if (err != HAL_IO)` to `if (1)`): caught at
    `test_profiles_http.c:1920`/`:1925`.
  - LOW-1 `io_seg_start` clear removed: caught at `test_profile_executor_prestart.c:13583`.
  - LOW-1 `leave_on` clear removed: caught at `test_profile_executor_prestart.c:13587`.
  - LOW-3 wrong-size favorites blob returns `HAL_OK`: caught at `test_profiles_http.c:1968`.

  The new tests do pin the fixed behavior. The gaps listed in INFO-2f are
  properties the tests do not cover, not vacuous tests.

## LOW-1: clearing `zone_off_pending_mask` (sound)

The question: can clearing a pending OFF leave a relay energized when an OFF
was owed?

- `io_seg_start` (`profile_executor_relay_io.c:1173`) clears only the segment's
  own bit, and only when `kiln_io_owner_command_set_relay_mask_authorized_since`
  returns `ESP_OK`. If an all-off ran since the sampled epoch, the owner turns
  the ON into an OFF and returns `ESP_ERR_INVALID_STATE`, so nothing is
  cleared. In that case the pending OFF stays owed, which is correct.
- A successful write means the hardware state now matches what the segment
  requested. Any earlier failed OFF on that bit is superseded. If the segment
  wrote OFF, the OFF is done. If it wrote ON, the segment owns the relay, and
  `io_seg_finish` owes the OFF.
- Every abnormal and non-RUNNING path calls `io_seg_finish(false)`: stop,
  abort, fault and pause, through `io_segs_force_all_off(false)` on every
  non-RUNNING tick (`profile_executor.c:754-767`). That path ignores
  `leave_on_at_end` and re-records a failed OFF at line 1231. So a fault
  cannot strand an ON that the pending mask has forgotten.
- `io_seg_finish` (line 1220) clears pending only when `leave_on` is true.
  `leave_on` requires `honor_leave_on`, which only the normal segment end and
  the completed-run end pass. That is the owner's explicit "leave this relay
  ON" request, so dropping a stale pending OFF is the intended result.

INFO-1a. The pending mask is not source-tagged. A pending bit set by another
writer on the same relay is also cleared by a successful segment write:
`force_relay_mask_off` (line 1561), the fallback (line 95), or
`force_all_relays_off` (line 653). For that to strand an owed OFF, the relay
would have to be a zone or aux relay and also an IO-segment relay at the same
time during a run. The relay/zone mode gate refuses that reassignment while a
run is active (409), and `zone_off_pending_retry_running` excludes segment
relays anyway. Not reachable in practice; noted for future relay-sharing work.

INFO-1b. In the leave-on case, pending is cleared even when the segment's own
start write failed (state unknown). The relay is then neither confirmed ON nor
retried OFF. After a normal end the segment is no longer active, so no later
`io_seg_finish(false)` revisits it. This matches the owner's leave-ON intent,
so it is acceptable, but the comment at line 1219 could say that a relay in an
unknown state is left as it is.

## LOW-2: used-bitmap rebuild and the longer blob

Byte safety of the rewrite is fine. `used_bitmap_save` re-probes the stored
length, reads a 17..64-byte blob into `big[64]`, overlays the first 16 bytes,
and writes back the original length. A failed re-read returns `HAL_IO` and
writes nothing. Every caller holds the profiles save lock or runs at
single-threaded boot, so nothing else can change the blob between the read and
the write.

There is no new large task-stack buffer. `uint8_t big[64]` appears in
`used_bitmap_load` and in `used_bitmap_save`, never both live at once (64 B).
That is well under the httpd large-local concern. Note that review 7 moved the
rev array to `persist_scratch_alloc` because of the `bx_flash_worker` stack
ceiling, so check that worker's measured margin if it can reach these paths.

### LOW-2a: rebuild on any `HAL_IO` can drop a live slot, and the next boot then deletes its file

`used_bitmap_load_or_rebuild` (`profiles_http.c:902-919`) rebuilds whenever
`used_bitmap_load` returns `HAL_IO`. That is not only the wrong-size case.

- The 16-byte path returns the raw `hal_kv_get_blob` status.
- `hal_esp_err_to_status` maps every unrecognized ESP error to `HAL_IO`
  (`hal_kv_esp.c:178`), so a genuine flash or NVS read error triggers the
  rebuild too. The log line then says "unusable size", which is wrong.
- The rebuild scan treats every probe failure as "key absent". It only checks
  `hal_kv_get_blob(...) == HAL_OK`.

Scenario:

1. A slot was last saved in the dual-write era. Its migrated file has
   `file_rev == nvs_rev` (the migration writes `mig_rev = nvs_rev`), and the
   legacy `profN` key and bitmap bit are both still present, because retire
   only happens when the file rev is strictly greater.
2. A user deletes a different slot. `nvs_erase_slot_locked` calls the rebuild,
   which hits a transient `HAL_IO` on the bitmap read.
3. During the rebuild, the probe of this slot's `profN` key also fails (same
   flash fault window). Its bit is dropped, and the rebuilt bitmap is saved.
4. Next boot: the slot's bit is clear but its key is present. The slot is
   skipped, so `nvs_slot_valid=false` and `slot_blob_bad=false`, and the floor
   passed is `nvs_rev`.
5. `profiles_cfg_fs_resolve_ex` (`profiles_cfg_fs.c`, around line 238) sees
   `!nvs_valid` with `file_rev <= nvs_rev` and `nvs_rev != 0`. It treats the
   file as a stale leftover of a delete and calls `profiles_cfg_fs_delete`.
   The profile is lost.

This needs two read faults close together, so it is LOW. The non-rebuild boot
path deliberately avoids exactly this outcome: a present-but-unreadable key
sets `slot_blob_bad`, and the floor drops to 0.

Fix:

- Rebuild only on the wrong-size condition. Give `used_bitmap_load` a distinct
  result for "present at an unusable size", and pass a real read error through
  as before.
- In the scan, set the bit on `HAL_OK`, leave it clear only on `HAL_NOT_FOUND`,
  and fail closed (return the error, save nothing) on any other probe status.

### LOW-2b: any 17..64-byte blob is now accepted as a bitmap, including junk, and that can resurrect a deleted slot

`used_bitmap_load` now accepts any length from 17 to 64 bytes as a
"longer, newer-firmware" bitmap, with no structural check. The rev-array
loader at least requires `len % sizeof(uint32_t) == 0`. Before `b76d03657`, a
blob of that size failed closed at boot into the files-only path. Now it loads
at boot as a valid bitmap.

Scenario:

1. A corrupt or junk 20-byte `used` blob has stray bits set for slots that
   have no `profN` key.
2. Boot visits those slots. `hal_kv_get_blob` returns `NOT_FOUND`, so
   `slot_blob_bad` is true and the floor passed to resolve is 0.
3. One of those slots was deleted earlier, but its file removal failed, so a
   stale file is still there. Resolve sees `!nvs_valid` and `nvs_rev == 0`
   (the floor was forced to 0), and adopts the file. The deleted profile comes
   back.

Before the fix, the junk-length blob made boot take the files-only path,
which uses the persisted rev floors and would have deleted that stale file.

Fix: accept the longer shape only when `len % 4 == 0`, matching
`profiles_slot_bitmap_t`'s word layout and the rev-array rule. Optionally also
require that the bits above `PROFILES_MAX_COUNT` in the first 16 bytes are 0.
Treat anything else as wrong-size (rebuild on the save/delete path, fail
closed at boot).

### INFO-2c: a blob longer than 64 bytes loses its tail

Over 64 bytes, `used_bitmap_load` returns `HAL_IO`, then the rebuild runs, and
`used_bitmap_save` writes a 16-byte blob, dropping the tail. That only matters
for a future firmware with more than 512 slots. Fine, but the comment should
say that the 64-byte cap is a deliberate limit.

### INFO-2d: the rebuild zeroes bits 100..127

The rebuild clears the bits for slots 100..127 inside this build's own 16
bytes, which a newer firmware could own. That is harmless today and the same
as what `used_bitmap_save` did before, but it differs from the longer-blob
path, which preserves foreign data.

### INFO-2e: the rebuild drops a dangling bit, unlike the boot path

Take a dangling bit (bit set, key absent) left by an earlier delete that only
partly failed. The rebuild drops that bit. The next boot then treats the slot
as deleted with floor `nvs_rev` and removes any file. That is the user's
requested delete finishing, so it is acceptable. But it differs from the
non-rebuild boot path, where `slot_blob_bad` keeps the file. Worth a sentence
in the comment.

### INFO-2f: test gaps

- `test_nvs_erase_slot_repairs_wrong_size_used_bitmap` checks only that the
  blob becomes 16 bytes. It never checks that a legacy slot whose `profN` key
  is present keeps its bit after the rebuild. That property is what protects
  against LOW-2a-style loss.
- There is no boot-load test of a longer blob (the new boot behavior), no
  junk-length test (LOW-2b), and no test that a non-wrong-size read error
  passes through without a rebuild.

## LOW-3: favorites wrong-size blob returns `HAL_IO`

Boot is not broken. The only startup caller, `profiles_favorites_start` in
`main_network_http.c:417`, is non-fatal. It logs the error and calls
`startup_fault_note(STARTUP_FAULT_SETTINGS_STORE)`. If a cfg file exists,
`profiles_favorites_start` adopts the file and returns `ESP_OK`, so the NVS
error never surfaces. `get_dualwrite_status` just reports `nvs_valid=false`.
No other caller treats the error as fatal.

### LOW-3a: favorites are still silently reset by the next write

The fix says a wrong-size blob must never silently read as "nothing
favorited". But:

1. When there is no cfg file, `favorites_read_nvs` zeroes BOTH the user mask
   and the builtin mask on error. It skips the builtin read even though the
   builtin key is independent and readable.
2. RAM is therefore empty.
3. `profiles_favorites_set` writes a file at `s_fav_rev + 1` from that RAM.
   The first star toggled by the user therefore persists "only this one
   favorite", and the file then beats NVS on every later boot.

So the reset is deferred from "on load" to "on the next write", and it now
also loses the readable builtin favorites. The startup fault note is the only
signal.

Fix: read the builtin mask independently of the user-mask error. Then either
refuse `profiles_favorites_set` while the user mask is unresolved (409/500
with a clear message), or merge the change into a re-read rather than writing
from zeroed RAM.

## INFO-5: commit recheck message (sound)

`profile_executor_run.c:1440` now passes `err_msg`/`err_cap` straight to
`relay_authority_start_blocked`, so the refusal text is the gate's own text
("safety link is down or not yet confirmed up") rather than a second,
divergent copy. The test expectation was updated to match. No behavior change.
