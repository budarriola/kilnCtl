# Review: firelowfx2 fixes (2026-10-10)

Reviewed commits on origin/dev:

- `a118a9df1` profiles: rebuild the used bitmap only on a wrong-size blob, fail closed on probe errors, accept only word-multiple longer blobs; favorites set refuses while the user mask is unresolved (REVIEW_FIRELOWFX LOW-2a/2b/3a)
- `ebdff64ee` profiles: SAVE_AS rollback deletes only a file this save wrote; `%2B` literal-plus tests (REVIEW_MISC8FX MED-1, LOW-1, LOW-2)
- `ce83e1ee3`, `5f687382b` test follow-ups
- `ee5466e6e` docs: mark the findings fixed

Reviewer: Opus, adversarial, read-only. No code changed.

## Verdict

The MISC8FX MED-1 fix is correct: a SAVE_AS rollback can no longer delete a file that another save wrote. The FIRELOWFX LOW-2a and LOW-3a fixes are correct in what they refuse. The `%2B` tests (MISC8FX LOW-1/LOW-2) are correct. Three things remain:

- LOW-2b is only half fixed. The load side enforces the word-multiple rule, but the save side does not.
- A 1-byte bitmap blob still reads as "no bitmap".
- The favorites refusal is a permanent, mislabelled outage.

There are no HIGH findings.

| ID | Grade | Summary |
|----|-------|---------|
| MED-1 | MED | An unresolved favorites mask is an outage that lasts until a reboot that succeeds. The HTTP path returns 500 "could not be saved to flash" because the store is never registered as degraded. With a persistent bad key, the only way out destroys every profile. |
| LOW-1 | LOW | `used_bitmap_save()` has no mod-4 check. A rebuilt 17..63-byte junk blob is written back at its junk length, so the repair never converges. Demonstrated by negtest. |
| LOW-2 | LOW | A 1-byte used blob still reads as "no bitmap". This is pre-existing (`01c74a3cc`). Demonstrated by negtest. |
| LOW-3 | LOW | Profile-store fail closed: two simultaneous corruptions refuse every delete until a profiles factory reset. |
| INFO-1..5 | INFO | Rollback edge cases that are harmless or unreachable, the SAVE_AS first-free-slot choice, and a stale favorite bit. |

## Answers to the review questions

**Can a SAVE_AS rollback still delete a file another save wrote?** No. The rollback (`profiles_http_save_ex`, `profiles_http.c` ~2029) deletes the target file only if all three of these hold:

1. The slot's rev is known (`!s_profile_rev_unknown[target_id]`).
2. The file decodes (`file_valid`).
3. Its rev equals the rev this save attempted, `s_profile_rev[target_id] + 1`.

Each case below was checked separately:

- **Concurrent save of the same rev.** Impossible. Every profile file writer (`nvs_save_slot_locked`, `nvs_erase_slot_locked`, retarget, `backup_import`'s profile commit) runs under `profiles_save_lock`. The rollback runs under that same lock, in the same critical section as the failed save. No other writer can bump the slot to the same rev between the failed write and the check. `nvs_save_slot_locked` advances the RAM rev only on success, so `s_profile_rev + 1` is exactly the rev that was attempted.
- **A file left by an earlier writer in a "free" slot.** At boot, any valid file in a slot with no RAM profile is adopted, through `profiles_cfg_fs_resolve_ex` (floor `nvs_rev`) or the files-only path (floor 0). So a free slot holding a valid file can only exist when its rev is unknown, and guard 1 covers that case. Both guards are load-bearing: negtest mutations that drop either one are CAUGHT by `test_rvfx_rollback_keeps_unexamined_file` and `test_rvfx_rollback_keeps_file_with_other_rev`.
- **Rev wrap.** At `UINT32_MAX`, both `nvs_save_slot_locked` and the rollback compute `+1` in `uint32_t` and both get 0, so they stay consistent. It is also unreachable in practice, because it needs 4e9 saves to one slot.

**Does fail closed leave the profile store permanently unusable?** No. It refuses only deletes (`nvs_erase_slot_locked`, through `used_bitmap_load_or_rebuild`), and only when two corruptions are present at once (LOW-3). Saves through the cfg file path do not depend on the bitmap rebuild. The favorites refusal is a separate case and is the real outage (MED-1).

**Does favorites INVALID_STATE surface sensibly?** Not on HTTP: it comes back as a 500 with the wrong text (MED-1). The LCD picker (`ui_page_profile_picker.c:209`) only logs a warning, which is acceptable for a background clear after a delete. The delete path (`profiles_http.c:2192`) ignores the result with `(void)`, so the delete proceeds. See INFO-5 for the side effect.

**Is the mod-4 rule right for every bitmap blob version?** Yes, on the load side. The used key has existed in two forms: a legacy u8 scalar, read through `hal_kv_get_u8` after a blob probe returns NOT_FOUND, and since `c61e3d378` the 16-byte `profiles_slot_bitmap_t` (`uint32_t words[4]`). Any future longer bitmap is a whole number of `uint32_t` words, so `len % 4 == 0` and `17..64` (in practice 20..64) is the correct test for "newer firmware". The save side does not apply the same rule (LOW-1), and the 1-byte blob case is mishandled (LOW-2).

**Are there reset-one-side issues between RAM slot state and files?** None new. The rollback leaves RAM untouched (the rev is not advanced on failure) and removes only the file it wrote, so the two stay paired. INFO-5 is a minor favorites-versus-slot drift.

## Findings

### MED-1: unresolved favorites is a permanent, mislabelled outage

`profiles_favorites_start()` (`profiles_favorites.c` ~240) sets `s_fav_user_unresolved = true` when the legacy NVS read fails and no cfg file resolves. `profiles_favorites_set()` (~283) then returns `ESP_ERR_INVALID_STATE` for every call, including built-in favorite toggles. Refusing the write is the right call, because the file holds both masks and writing it would persist an empty user mask. Three things are wrong around it:

1. **It is not registered as degraded.** Nothing calls `cfg_fs_degraded_set(PROFILES_FAVORITES_FILE_PATH, true)`. `pref_cfg_fs_resolve()` marks a path unknown only when the *file* is unreadable, and `favorites_read_nvs()`'s error is folded into `nvs_valid=false` before resolve sees it. So `cfg_fs_http_persist_failed_for()` (`profiles_edit_http.c:1014`) sends `500 {"error":"could not be saved to flash"}` instead of `409 store_unreadable_at_boot`, and `GET /api/cfgfs` does not list the store. The operator is told the flash write failed, when no write was even attempted.
2. **There is no retry.** The flag is evaluated once, at start. A transient error (for example `hal_kv_init_partition` failing early in boot) disables favorites until the next reboot. A persistent bad `prof_favusr`/`prof_favbi` key (wrong size or type) disables them forever.
3. **The only way out is destructive.** Nothing erases just the favorite keys or writes a fresh favorites file. `factory_reset(scope=profiles)` erases the whole `profiles_nvs` partition and every user profile with it.

Fix:

- Register the store as degraded at the same point the flag is set, and clear it again when the flag clears. Then `POST` favorite returns 409 and `GET /api/cfgfs` names the store.
- Re-attempt the NVS read on `profiles_favorites_set()` while the flag is set, so a transient failure heals without a reboot.
- Give a non-destructive way out. Either an explicit "reset favorites" action that writes an empty favorites file at rev 1 and erases the two legacy keys, or a factory reset scope limited to favorites.
- Add a test asserting a 409, not a 500, on the HTTP path.

### LOW-1: `used_bitmap_save()` rewrites a junk-length blob at its junk length

`a118a9df1` added `(len % sizeof(uint32_t)) == 0` to the longer-shape test in `used_bitmap_load_ex()` (`profiles_http.c:837`). An 18-byte blob is now classed `wrong_size`, and `used_bitmap_load_or_rebuild()` rebuilds it from the profN keys, as intended. `used_bitmap_save()` (~895) never got the same check. Any `cur_len` in 17..64 counts as a "newer-firmware longer bitmap", so it rewrites the first 16 bytes and writes the blob back **at 18 bytes**.

The repair therefore never converges:

- Every later delete rebuilds again.
- Every boot's `used_bitmap_load` returns `HAL_IO`, so `nvs_load_all_from` falls back to the degraded files-only path. That path cannot see legacy NVS-only `profN` slots (profiles never migrated to a file stay invisible) and sets `profiles` degraded.

This is exactly the state LOW-2b was meant to end.

Demonstrated: a negtest mutation that changes the junk blob in `test_used_bitmap_rebuild_keeps_present_slot_bit` from 5 bytes to 18 bytes FAILS at `test_profiles_http.c:2013: 16-byte bitmap`, because the blob stays 18 bytes after `nvs_erase_slot(3)`.

Fix: apply the same predicate in `used_bitmap_save()`: `cur_len > 16 && cur_len <= USED_BITMAP_LONGER_MAX_BYTES && cur_len % 4 == 0`. Better still, share one `used_bitmap_len_is_longer_shape()` helper between load and save so the two cannot drift again. Add the 18-byte case above as a permanent test.

### LOW-2: a 1-byte used blob reads as "no bitmap" (pre-existing)

In `used_bitmap_load_ex()`, the `wrong_size` branch excludes `len == 1` and falls through to `hal_kv_get_u8()`. A genuine 1-byte *blob* is a type mismatch for a u8 read, so it returns `HAL_NOT_FOUND` on target, and in the typed fake as well, as the comment just above says. That reads as "no bitmap yet".

Consequences:

- At boot, every slot is skipped, and `profiles_cfg_fs_resolve_ex` runs with `nvs_valid=false`. It deletes a dual-write-era file where `file_rev <= nvs_rev`, the same loss class LOW-2a/2b closed for other lengths.
- On delete, an empty bitmap is saved, which drops every other legacy slot's bit.

The trailing comment ("the host fake's size-based equivalent (err == HAL_OK, len == 1)") is stale. Since the typed fake, `len == 1` from a blob probe means a real 1-byte blob, never the legacy u8 format; on target, the legacy u8 format arrives through the first `HAL_NOT_FOUND` branch.

No firmware ever wrote a 1-byte blob, so this needs corruption to reach. That is why it is LOW.

Demonstrated: a negtest mutation that changes the junk blob in the same test to 1 byte (`0x28`, slots 3 and 5) FAILS at `test_profiles_http.c:2015: slot 5 (key present) keeps its bit`.

Fix: drop the `len != 1` exclusion, so a 1-byte blob is `wrong_size`. Then delete the fall-through u8 read, which is reachable only for `err` values other than OK/NOT_FOUND, or keep it only for `HAL_INVALID_ARG`. Update the comment.

### LOW-3: two corruptions block every delete until a profiles factory reset

`used_bitmap_load_or_rebuild()` fails closed when any profN probe returns an error other than OK or NOT_FOUND. That is correct: guessing would drop a live slot's bit. But it fails closed on every delete for as long as both corruptions persist, a wrong-size bitmap plus one unreadable profN key. The only way out is `factory_reset(scope=profiles)`, which loses every profile. Saves are not affected.

This is acceptable as LOW, given that it needs two independent corruptions. Two improvements would help:

- Log the slot id whose probe failed. Today the error is returned bare.
- Expose the condition in `GET /api/cfgfs`, so an operator can tell this state apart from a flash failure.

### INFO-1: a rollback keeps a byte-mismatched file

If `cfg_fs_write_atomic`'s read-back verify fails because the bytes differ, the file on flash is probably undecodable, so `file_valid=false` and the rollback now keeps it. This is harmless: boot ignores an undecodable file (`load_raw` valid=false), and the next successful save at the same rev replaces it.

### INFO-2: the rollback does flash I/O after a PSRAM-stack refusal

If `nvs_save_slot_locked` refused because `caller_stack_is_external()`, the rollback still calls `profiles_cfg_fs_load_raw`/`profiles_cfg_fs_delete`, and those have no PSRAM-stack guard of their own. This is unreachable today: the only caller that passes `out_persisted` with a fresh slot, `profiles_live_http.c:671`, runs on the httpd worker's internal stack. The pre-fix code made the same call too. A cheap guard would be to skip the rollback when the save was refused before any write.

### INFO-3: rev wrap

Covered in the answers above. Consistent and unreachable.

### INFO-4: SAVE_AS picks the first free slot even when its rev is unknown

If the first free slot has `s_profile_rev_unknown` set, every SAVE_AS is refused (`nvs_save_slot_locked`), even though other free slots could be written. This is pre-existing and not introduced here. Skipping rev-unknown slots in the free-slot search would fix it.

### INFO-5: a stale favorite bit after a refused delete-time clear

The delete path's `(void)profiles_favorites_set(id, false)` (`profiles_http.c:2192`) is refused while favorites are unresolved (MED-1). After a reboot that resolves favorites, the deleted slot's bit is still set, so the next profile saved into that slot shows as a favorite. This is cosmetic. Fixing MED-1's retry covers it.

## Testing

- `firmware\KilnFW\App\test\build_host_tests.ps1 -Only '^(profiles_http|profiles_live_http|profiles_builtin|profile_executor_prestart|dashboard_settings_http|setup_progress_http)( |$)'` on `ee5466e6e`: 6/6 built and passed.
- `tools\negtest.ps1 -Preset kilnfw-host -Parallel 2 -Mutations <json> -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"` on base `ee5466e6e`: the baseline passed, and all 4 mutations were CAUGHT; the real tree was unchanged.
  - `demoA_18byte_junk_not_rewritten_16` (LOW-1 demonstration): `test_profiles_http.c:2013: 16-byte bitmap`.
  - `demoB_1byte_blob_reads_absent` (LOW-2 demonstration): `test_profiles_http.c:2015: slot 5 (key present) keeps its bit`.
  - `guard_drop_rev_unknown` (rollback guard 1 replaced by `true`): `test_profiles_http.c:2489`.
  - `guard_drop_rev_match` (rollback rev-equality check dropped): `test_profiles_http.c:2505`.
