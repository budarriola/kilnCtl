# Review: firelowfx4 fixes (2759b48bf, f0d4eef3a), 2026-10-12

Scope: origin/dev `2759b48bf` and `f0d4eef3a`. Together they close
`docs/audits/REVIEW_FIRELOWFX3_2026-10-11.md` LOW-1, LOW-2, LOW-5, INFO-1 and INFO-6:

- A new `cfg_fs_condition_*` registry, rendered as `GET /api/cfgfs` `"conditions"`.
- `profiles_used_bitmap` moves out of the degraded registry and into the new one.
- The favorites POST has its own 409 text (`cfg_fs_refusal_http.h`).
- `favorites_load()` resolves into locals and publishes only on success.
- The redundant degraded clear is removed, and the boot log line is reworded.

This review is adversarial and read-only. It ran in worktree `C:\wt\pfx2rev_9ku948`,
which was minted from origin/dev and sits at `5f24cf7c6`, a descendant of both commits.
Nothing in the firmware was changed.

## Verdict

No HIGH, no MED. The registry is sound. It has the right bounds, takes the right
lock, and copies names into the table, so their lifetime is not a concern. The
LOW-2 publish fix does what it says.

There are three LOWs:

- The new favorites 409 text is wrong for one of the two causes that reach it (LOW-1).
- The `/api/cfgfs` worst-case buffer test never fills the degraded or conditions
  arrays. With both arrays full, the response does not fit the handler buffer and
  the route answers 500 (LOW-2).
- No consumer shows the new `conditions` array (LOW-3).

Adding the key breaks no consumer.

## Evidence

- **KilnFW host suite**, full run (`build_host_tests.ps1`): all 98 executables built and passed.
- **`check_00_kilnfw_target_build.ps1`**: PASS (`KilnCtrl.bin` produced).
- **PcTools, cfgfs-related tests**, run in the worktree with `PYTHONPATH` pointed at
  the worktree's `src`. Files: `test_mcp_server_cfgfs_format.py`,
  `test_full_board_backup_cfgfs.py`, `test_cfgfs_file_validate_scan.py`,
  `test_bench_test_cases_web_diag.py`, `test_mcp_server_backup.py`.
  Result: 105 passed.
- **Negtest**: `tools\negtest.ps1 -Preset kilnfw-host -Mutations <appendix>` with
  `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"` and `-Parallel 3`.
  log dir `%TEMP%\negtest_logs\20261012_004848_w0sa`. The unmutated baseline passed (807 s).

| Mutation | Verdict | Where caught |
|---|---|---|
| M1 `conditions` never rendered | MISSED | |
| M2 `profiles_used_bitmap` condition never cleared | MISSED | |
| M3 condition clear is a no-op | MISSED | |
| M4 unresolved path zeroes the user mask | MISSED | |
| M5 unresolved path zeroes the builtin mask | CAUGHT | `test_profiles_http.c:2176`, `:2180`, `:2188` |
| M6 a successful load keeps `s_fav_user_unresolved` | MISSED | |
| M7 favorites POST reverted to the generic 409 | CAUGHT | `:2209`, `:2210`, `:2213` |
| M8 unresolved path not marked degraded | CAUGHT | `:2203`, `:2207`-`:2212` |
| M9 probe error does not set the condition | CAUGHT | `:2130` |
| T1 worst-case test with 6+6 maximum-length names | NOT RUN (still in flight when this review landed) | |

Mutations M1-M9 are on firmware. T1 changes only the test, to measure the buffer with the degraded and conditions arrays filled to their caps.

## Findings

### LOW-1: the favorites 409 text is wrong when the cfg file, not the NVS key, is unreadable

`cfg_fs_http_persist_failed_favorites()` sends `CFG_FS_FAVORITES_UNREADABLE_REASON` when all of these hold:

- cfg is mounted
- `prof_fav.bin` is degraded
- the error is `ESP_ERR_INVALID_STATE`

The text assumes the cause is the NVS user key (`s_fav_user_unresolved`). Two causes reach that 409:

1. **NVS user key unusable, no cfg file.** `favorites_load()` sets
   `s_fav_user_unresolved` and the degraded mark. `set()` re-runs the load on every
   request. The text is right for this case.
2. **`prof_fav.bin` unreadable** (a read I/O error or an allocation failure in
   `resolve_with_file()`). `pref_cfg_fs_resolve()` calls `unknown_set(path, true)`,
   which marks the path both degraded and rev-unknown.
   - If NVS holds a valid copy, the M1 path adopts it.
   - If NVS has no copy (the normal case since the dual-write close),
     `favorites_load()` takes the "no favorites saved yet" branch. That branch
     publishes zero masks and clears `s_fav_user_unresolved`.
   - Either way, `set()` never reloads. `pref_cfg_fs_save()` refuses every save
     with `ESP_ERR_INVALID_STATE` ("unreadable at boot ... reboot to retry"), so the
     client gets the favorites 409.

For cause 2 the text is wrong on three counts:

- It says "this request re-reads it each time, so retry". In fact only a reboot re-reads the file.
- It says "neither a cfg format nor a factory reset with scope kiln clears it". A
  cfg format does clear it: it erases the unreadable file.
- It says "only factory_reset scope=profiles". `scope=all` also erases
  `profiles_nvs` and formats cfg. This last point applies to cause 1 as well.

Cause 2 has a second symptom. With no NVS copy, the favorites a user really has
saved show as "none" this boot. Nothing is lost, because every save is refused and
the file is not touched. However, a `set()` in this state changes RAM and then
fails. It leaves `s_fav_dirty` set and RAM holding a one-bit mask that matches
neither the file nor the empty display.

**Fix:**

- Pick the reason by cause. If `pref_cfg_fs_rev_unknown(PROFILES_FAVORITES_FILE_PATH)`
  is true, send the generic "unreadable at boot, reboot to retry; a cfg format
  discards it" text. Send the favorites text only when the user mask is unresolved.
  For example, expose `profiles_favorites_user_unresolved()` and test it in the handler.
- Mention `scope=all` next to `scope=profiles` in the favorites text.
- In `favorites_load()`, treat `have == false` together with
  `pref_cfg_fs_rev_unknown(path)` as unresolved, not as "no favorites saved yet".
  That way the masks are not published as zero for an unreadable file.
- Add a test for both causes. The host fake already has an injected cfg read error.

### LOW-2: the `/api/cfgfs` worst-case buffer test omits the degraded and conditions arrays

`test_worst_case_fits_handler_buffer` renders 20 dual-write rows, 18 files, a
failed format and a full window. It sets no degraded name and no condition. Its
measured length is 5137 B, or 5167 B with the longest `esp_err` name. That leaves
465 B of the 5632 B `CFG_FS_STATUS_HANDLER_JSON_BUF`.

Both arrays list up to 6 names of up to 47 characters each, about 300 B per array.
Mutation T1 (appendix) adds 6 degraded names and 6 condition names of 46 characters each to
that test, and the render then does not fit. That is arithmetic, not a measured result: the mutation run was still in flight when this review landed. 465 B of margin minus about 600 B for the two arrays (6 x 48 B + 6 x 48 B, plus quotes and commas) is negative. When the render does not fit,
`cfgfs_status_get_handler` answers 500 "status build failed". So the
diagnostics route fails exactly when there is the most to diagnose.

The degraded half of this predates these commits. These commits made it reachable
in a second way.

**Fix:** fill both arrays to their caps with maximum-length names in the worst-case
test. Then either raise `CFG_FS_STATUS_HANDLER_JSON_BUF` by at least 700 B (it is
heap scratch, not stack), or cap the listed names by remaining space and keep the
exact count. The second option needs a `conditions_count` field (see INFO-1).

### LOW-3: no consumer shows `conditions` (or `degraded`)

The added key breaks nothing. Every consumer reads only named fields:

- `diagnostics_page.html`: `mounted` and the item rows.
- MCP `get_cfgfs_status` (`mcp_server_info.py`): mounted, capacity, files, subdirs, dual_write.
- bench `cases_web_diag`: `mounted`.
- `test_full_board_backup_cfgfs.py`'s parsers: the files list.

Backup export's `stale_or_unknown_stores` now no longer lists
`profiles_used_bitmap`. That is the intended effect of the FX3 LOW-5 fix.

However, neither the web page nor the MCP tool renders `degraded` or `conditions`.
A latched `profiles_used_bitmap` condition, or a degraded store, is therefore
visible only to someone who reads the raw JSON. The FX3 LOW-5 fix moved the
signal into a field nobody reads.

**Fix:** have `get_cfgfs_status` print `degraded_count`/`degraded` and `conditions`
whenever they are non-empty, and add the same line to the diagnostics page's cfg card.

### INFO-1: `conditions` is capped at 6 against a table of 8, with no count field

The status loop stops at `i < 6`, but `CFG_FS_CONDITION_MAX` is 8. `degraded` carries
an exact `degraded_count`; `conditions` does not, so a 7th or 8th condition would be
silently omitted. Only one condition exists today.

The Fix status line also says the condition "no longer uses the 6/8 listing caps".
That is true of the degraded caps, but `conditions` has its own cap of 6.

**Fix:** add `conditions_count`, or loop to `CFG_FS_CONDITION_MAX`.

### INFO-2: a leading comma is possible in both arrays

The status loops use `i == 0 ? "" : ","`. If `cfg_fs_*_name(0, ...)` fails because a
concurrent clear compacted the table, and `name(1, ...)` succeeds, the array starts
with `,"x"`, which is invalid JSON. This needs a set or clear between `count()` and
`name()`, so it is rare. It is the same in `degraded`.

**Fix:** track an `emitted` flag instead of testing `i`.

### INFO-3: the `profiles_used_bitmap` condition can stay latched after the bitmap is cured another way

The condition is set on a profN probe error and cleared only after a successful
wrong-size rebuild. If the bitmap later loads at the right size without going
through the rebuild, the condition stays until reboot. The early return
`err != HAL_IO || !wrong_size` never clears it. Today only `used_bitmap_save` writes
the blob, after a successful load, so this is narrow.

Nothing else reads the condition. Readiness, the LCD, bench cases and PcTools do
not reference `profiles_used_bitmap`, so removing it from the degraded registry
changed no other behavior.

**Fix (optional):** clear the condition on any successful bitmap load.

### INFO-4: a missing builtin key keeps the stale RAM mask in the unresolved path

`favorites_read_builtin_only()` updates `*builtin` only on `HAL_OK`. On a real
`HAL_NOT_FOUND` (the key was erased, so the true value is 0), the unresolved path
keeps the previously published builtin mask. The new test asserts that behavior
only for a readable key. This is harmless while the store refuses writes.

**Fix (optional):** set `*builtin = 0` on `HAL_NOT_FOUND`.

### INFO-5: lock-free readers can see a torn publish

`profiles_favorites_is()` and `_masks()` read `s_fav_user` (16 B) and `s_fav_builtin`
without `s_save_lock`. The success publish writes them one after the other, so a
reader can see the new user mask with the old builtin mask. The display is
momentarily mixed and nothing is persisted. This is acceptable. It is also no worse
than the pre-fix code.

### INFO-6: test coverage gaps found by mutation

See the negtest table below. Mutations M1, M2, M3, M4 and M6 were MISSED.

- M1: nothing asserts that `conditions` is rendered in the JSON.
- M2 and M3: nothing asserts that a condition clears.
- M4: the LOW-2 test checks only the builtin mask, never `s_fav_user`.
- M6: nothing asserts that a successful load resets `s_fav_user_unresolved`. The
  heal test does pass, because `set()` clears the flag through its own reload.

**Fix:** add `test_cfg_fs_status` cases for the `conditions` render and for a clear,
and assert the user mask in `test_favorites_refused_retry_keeps_published_masks`.

### INFO-7: the FX3 review doc's negtest line has a mangled path

In `REVIEW_FIRELOWFX3_2026-10-11.md` Fix status, the text `tools\negtest.ps1` reads
`tools` followed by a line break and `egtest.ps1`, because `\n` was expanded.

### Confirmed sound

- **Registry thread safety.** Set, count, name and is all run under `s_degraded_mux`.
  The overflow `ESP_LOGE` runs after the critical section is left.
- **Registry bounds.** Names are `strncpy`'d to 47 characters and NUL-terminated.
  The table compare uses the same 47-character limit.
- **Name lifetime.** Names are copied into the table, so a caller's buffer is not
  referenced afterward.
- **Clear-for-test.** `cfg_fs_degraded_clear_for_test()` also clears `s_condition`.
- **INFO-1 removal.** `pref_cfg_fs_resolve()` reaches `unknown_set()` on every path
  for a 20-byte item (including the scratch-alloc path), so the removed clear was
  redundant.
- **LOW-2 publish.** Locals are published only on success. The "no favorites" path
  writes zero masks, which is the true state when both NVS and the file are absent;
  LOW-1 covers the file-unreadable variant. The unresolved path starts the builtin
  re-read from the published value.
- **Recovery image.** It links none of these files.

## Fix status

All findings fixed on origin/dev in the commit that adds this table (pfx2fix). Negtest: 15 of 15 mutations CAUGHT (the reviewer's M1-M9, M1b, plus L2 buffer shrink, L1 and L1b text/unreadable-file mutations and I4), baseline passed; full KilnFW host suite 98/98 passed.

| Finding | Status | Fix |
|---|---|---|
| LOW-1 | FIXED | Favorites 409 now names `factory_reset scope=profiles` or `scope=all` (both erase every user profile). When prof_fav.bin itself is unreadable (rev-unknown) the 409 falls back to the generic text (a reboot retries, a cfg format clears it) and `favorites_load` stays unresolved instead of publishing a zero mask. Retry now heals once the read works. |
| LOW-2 | FIXED | `CFG_FS_STATUS_HANDLER_JSON_BUF` 5632 to 6656 B (heap scratch). Worst-case test fills 9 degraded and 8 conditions with 47-char names: 5856 B rendered (5886 B with the longest esp_err name), at least 400 B margin. Fails against the old 5632 B. |
| LOW-3 | FIXED | Diagnostics page shows "Stores refused this boot" and "Runtime conditions"; MCP `get_cfgfs_status` prints degraded and conditions lines. Python tests added. |
| INFO-1 | FIXED | Conditions loop lists all (table max 8) and emits `conditions_count`. |
| INFO-2 | FIXED | `emitted` flag replaces `i == 0` for comma placement in both loops. |
| INFO-3 | FIXED | `used_bitmap_load_or_rebuild` clears `profiles_used_bitmap` on any successful load as well as after rebuild. |
| INFO-4 | FIXED | Builtin key HAL_NOT_FOUND sets the mask to 0 rather than keeping the stale RAM value. |
| INFO-5 | FIXED | Both masks published together through `fav_publish()` under a spinlock; readers copy under it. |
| INFO-6 | FIXED | Tests added so M1, M2, M3, M4, M6 are CAUGHT (see negtest results above). |
| INFO-7 | FIXED | Typo in REVIEW_FIRELOWFX3 (`tools` newline `egtest`) restored. |

## Appendix: negtest mutations (JSON)

```json
[
 {
  "name": "M1_conditions_never_rendered",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs_status.c",
  "find": "for (int i = 0; i < cn && i < 6; i++) {",
  "replace": "for (int i = 0; i < 0; i++) {",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M2_bitmap_condition_never_cleared",
  "file": "firmware/KilnFW/App/drivers/http/profiles_http.c",
  "find": "    cfg_fs_condition_set(\"profiles_used_bitmap\", false);",
  "replace": "    (void)0;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M3_condition_clear_noop",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs.c",
  "find": "                s_condition[i][0] = '\\0';",
  "replace": "                (void)0;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M4_unresolved_zeroes_user_mask",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "        s_fav_user_unresolved = true;",
  "replace": "        s_fav_user_unresolved = true; profiles_slot_bitmap_from_u32(&s_fav_user, 0);",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M5_unresolved_zeroes_builtin_mask",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "        s_fav_builtin = builtin;",
  "replace": "        s_fav_builtin = 0; (void)builtin;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M6_success_keeps_unresolved_flag",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "    s_fav_user_unresolved = false;\n    ESP_LOGI(TAG, \"favorites: saved",
  "replace": "    ESP_LOGI(TAG, \"favorites: saved",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M7_favorites_409_reverted_to_generic",
  "file": "firmware/KilnFW/App/drivers/http/profiles_edit_http.c",
  "find": "cfg_fs_http_persist_failed_favorites(req, PROFILES_FAVORITES_FILE_PATH, err)",
  "replace": "cfg_fs_http_persist_failed_for(req, PROFILES_FAVORITES_FILE_PATH, err)",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M8_unresolved_not_marked_degraded",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "        cfg_fs_degraded_set(PROFILES_FAVORITES_FILE_PATH, true); /* FIRELOWFX2",
  "replace": "        (void)0; /* FIRELOWFX2",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M9_probe_error_condition_not_set",
  "file": "firmware/KilnFW/App/drivers/http/profiles_http.c",
  "find": "            cfg_fs_condition_set(\"profiles_used_bitmap\", true);",
  "replace": "            (void)0;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "T1_worst_case_with_max_degraded_and_conditions",
  "file": "firmware/KilnFW/App/test/test_cfg_fs_status.c",
  "find": "    static char json[WORST_CASE_HANDLER_BUF];\n    size_t len = 0;",
  "replace": "    cfg_fs_degraded_clear_for_test();\n    for (int k = 0; k < 6; k++) { char nm[48]; snprintf(nm, sizeof(nm), \"degraded_store_name_padded_to_the_47_char_max%d\", k); cfg_fs_degraded_set(nm, true); snprintf(nm, sizeof(nm), \"condition_name_padded_out_to_the_47_char_maxx%d\", k); cfg_fs_condition_set(nm, true); }\n    static char json[WORST_CASE_HANDLER_BUF];\n    size_t len = 0;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 }
]
```

## INFO triage (2026-10-12)

- INFO-4: won't fix: the store refuses writes in the unresolved state, so a stale builtin mask cannot be persisted; the mask is display-only until the next successful load.
- INFO-6: won't fix: test-only coverage gaps (conditions render, clear, user mask, flag reset) for a diagnostic key nothing consumes; no safety path depends on them. Revisit if a consumer of `conditions` is added.
