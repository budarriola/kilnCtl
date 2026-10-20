# Review: FW-01..04 flash-wear fix (113bd76be), 2026-10-20

Scope: commit 113bd76be on origin/dev, the fix for FW-01..04 from `docs/REVIEW_FLASH_WEAR_FD_2026_10_19.md`.
This is a review only. Totals: HIGH 0, MED 2, LOW 5, INFO 6. KNOWN_ISSUES rows FV-01..FV-07.

## Checks run (worktree at origin/dev containing 113bd76be)
- `build_host_tests.ps1 -Only "boot_history|firing_trace|trip_history|diag_bundle|diagnostics|time_sync"`: all 10 executables passed (boot_history 66/66, firing_trace 68/68, trip_history 53/53, time_sync 95/95).
- `tools/check_internal_alloc_allowlist.ps1`: PASS (29 files).
- `tools/negtest.ps1 -Preset kilnfw-host`, 4 mutations, ALL_CAUGHT. Every mutant compiled and failed a test assertion: flush every boot (bh_always_persist); write on every merge (trip_pow2_always); never append (ft_no_append); reject the torn last batch instead of dropping it (ft_torn_reject).
- `firmware/KilnFW/App/test/check_all_task_stack_budgets.py`: NOT run. It measures a built `KilnCtrl.elf`, and agent rules forbid full target builds. The coordinator's batch `-Fast` run on the dev tip must cover it (FV-07).

## MED
- **FV-01 MED: coalesced boots share one boot seq, so the per-boot keys used downstream merge across reboots.**
  `boot_history_latest_seq()` returns `s_recent[0].seq`. A coalesced boot gets no new seq.
  - `trip_history` merges on `last->boot_seq == in->boot_seq`. In a boot loop (same reason and fw), the ESP uptime restarts near 0 and the boot seq does not change. A same-reason trip in the next boot within 60 s of the stored uptime then merges into the previous boot's entry. The check-3 guarantee ("60 s window does not cross a reboot") holds only for non-coalesced boots.
  - The `SYS_BOOT` marker arg (FQ-01) is also the same for each coalesced boot. The PC decoder splits on the marker, not on its value, so the split still works, but the boot seq can no longer name the boot.
  - Fix: give each boot a RAM-only boot counter for trip_history keying (for example seq plus repeat index, or an RTC_NOINIT boot nonce). Or include the reset reason and repeat in the trip key.
- **FV-02 MED: the wall-time backfill only runs inside a TIME_SYNC event, and the new 60 s gate can skip it for a whole sync interval.**
  The `uptime_s >= 60u` test sits in `telemetry_time_sync_service()` after `time_sync_ack_event()`. If SNTP syncs before 60 s of uptime (normal on home Wi-Fi, about 5 to 15 s), the event is acked and the backfill is skipped. `s_wall_backfilled` stays false, so it is retried only on the next sync event. With the lwIP SNTP default of a 1 h resync, the newest boot keeps `wall_time = 0` for about an hour, or forever on a boot shorter than that.
  - Fix: remember the offset and run the backfill on the first service pass with uptime >= 60 s, independent of a new event.
  - No host test covers the early-sync case. The existing test syncs after 60 s or not at all.

## LOW
- **FV-03 LOW: the coalescing key ignores `has_crash` and `prev_uptime_known`.**
  Only reason and fw are compared. A boot with a crash record (`has_crash = 1`) is folded into an earlier same-reason entry with `has_crash = 0`. Its crash flag, prev uptime and wall time are lost, and up to 15 such boots are unpersisted if power fails (RTC_NOINIT pending is lost on power-on).
  - A distinct reason or fw always persists at once, so the first crash of a new kind is never lost.
  - A repeated identical PANIC loop is not lost either: the first entry carries `has_crash`.
  - Fix: add `has_crash` to the key, cheaply.
- **FV-04 LOW: an unpersisted trip merge is lost on reboot.**
  Merges persist only at power-of-2 counts. For a count 129..255 storm, up to 127 merged trips (count plus the newest key/stamps/safety TC) exist only in the RAM shadow.
  - The first trip of every reason and boot is always written, so no trip KIND is ever lost. Check 3's "a different reason is always recorded" holds; tests cover it.
  - The newest stamps are lost. After a reboot, a Pico re-send of the latched trip does not match the persisted (older) key and is pushed as a new entry. That is acceptable, but it double-counts.
  - Documented in the header; listed for the record.
- **FV-05 LOW: a corrupted count field in a middle FTR2 batch reads as a torn tail.**
  `parse_file` breaks (accepts) when `cnt` is too large or `end > len`. So a bit flip that enlarges a middle batch's count silently truncates the file at that batch instead of rejecting it. Only a CRC mismatch with data after it is rejected.
  - Fix: treat `end > len` as torn only when the batch would be the last, that is, when the remaining bytes are fewer than a full batch header plus records. Or require the remaining length to be < FT_BHDR_SIZE + FT_FLUSH_EVERY*FT_REC_SIZE + slack. Otherwise return false.
- **FV-06 LOW: a UTF-8 BOM was added to `firmware/KilnFW/App/drivers/persist/firing_trace.c` line 1** (`\xEF\xBB\xBF// See firing_trace.h`). GCC accepts it, but it is an unrelated byte change and breaks the no-BOM convention. Remove it.
- **FV-07 LOW: stack budgets were not verified.**
  The fixer skipped `check_all_task_stack_budgets.py`, and this review could not run it (it needs a target ELF).
  - New stack use on the flash worker: `trip_history` flush_job keeps `shadow_save` (36 B) and a merge path. `boot_history_record_boot` still holds a full `boot_history_blob_t` (588 B, was 524 B) on the caller's stack. `cfg_fs_append` heap-allocates its path (fine).
  - The +64 B blob growth on the boot path should be confirmed against the boot task budget in the coordinator run.

## INFO
- **INFO-1: v1-to-v2 migration.** The v2 blob is 588 B and v1 is 524 B. `hal_kv_get_blob` returns the stored 524-B length, the `len != sizeof` test discards it, and the ring restarts at seq 1. All boot history before the upgrade is lost on the first v2 boot (accepted by the fix). Seq restarting at 1 can, in theory, equal an older trip_history entry's `boot_seq`. A merge then needs the same reason and an uptime within 60 s of the newest pre-upgrade trip: negligible. Host test covers the discard.
- **INFO-2: repeat overflow.** `repeat` is u16, saturated at 0xFFFF in both paths, and `pending` is bounded by FLUSH_EVERY. No overflow. The trip `count` also saturates at 0xFFFF; `is_pow2(0xFFFF)` is false, so a saturated storm stops writing (intended).
- **INFO-3: torn append and fallback.**
  - A failed append clears `s_on_flash`, so the next flush is a `cfg_fs_write_atomic` rewrite of the whole RAM buffer as one batch.
  - A power cut mid-append leaves a short or bad-CRC last batch, which the reader drops (tested). The init after a reboot lists the file as interrupted with the surviving samples (tested).
  - The close marker is a count-0 batch with FT_FLAG_CLOSED, and flags are OR-ed across batches.
- **INFO-4: readers.** `firing_trace_http.c` allocates `FT_FILE_MAX` (now 30,104 B, PSRAM) and goes through `firing_trace_open`, which compacts batches in place. The CSV path therefore sees contiguous samples. The diag bundle does not embed trace files. No PC tool parses FTR files.
- **INFO-5: the real wear win is smaller than "about 1x file size".**
  The host test counts bytes handed to the IO seam: under 2x, about 30 KB per firing versus 2.1 MB. On LittleFS, each fsync of an appended file rewrites the partially filled tail block, copy-on-write, plus metadata. So each of the ~145 appends costs up to one 4 KB block program, not 208 B: on the order of 0.3 to 0.6 MB per 24 h firing.
  - That is still a 4x to 7x cut, and the temp-file copy per flush is gone.
  - Erase-count wear drops from about 600 to about 75 to 150 blocks per firing.
  - Update the header comment's "about 1x" claim.
- **INFO-6: JSON output lacks the new fields.** Boot `repeat` and trip `count` are stored but not emitted by `GET /api/crash_report`, the `/diagnostics` tables or the diag bundle. A 99-boot loop shows as a single boot, and a 200-trip storm as one trip. Add `"repeat"` and `"count"` to the boot_history and trip_history JSON, and to the /diagnostics columns.
- **Tests: meaningful.** The write-count assertions (8, 10, and 2x bytes) are tight enough that all 4 mutants failed them. `fake_kv_set_call_count` counts every `hal_kv_set_blob` call.
  - Gap: no test for the early-SNTP backfill (FV-02).
  - Gap: no test for cross-boot trip merging under coalescing (FV-01).
  - Gap: no test for the middle-batch count corruption (FV-05).
