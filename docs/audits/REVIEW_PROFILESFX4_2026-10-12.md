# Review: REVIEW_PROFILESFX2 fixes (92e02babf, 3063da377), 2026-10-12

Reviewer: Opus. This reviews the commits that fix `REVIEW_PROFILESFX2_2026-10-12.md` LOW-1..3 and INFO-1..7:

- `92e02babf`: favorites masks published under a spinlock, no zero mask for an unreadable `prof_fav.bin`, the 409 text for that case, the cfg status buffer grown 5632 -> 6656 B, conditions rendered in web/MCP, tests.
- `3063da377`: the Fix status table.

Review tree: `C:\wt\rvprofilesfx4_lrnd51` at `3063da377` (origin/dev at the time).

## Verdict

No HIGH or MED findings. Two LOWs and seven INFO items. The spinlock is correct. The unresolved state heals as claimed. The NOT_FOUND mapping is sound. The 6656 B worst case is honest. Both LOWs are in the favorites "unreadable file" path that LOW-1 was about.

- LOW-A (fixed in `bebc85025`; see `REVIEW_PROFILESFX6_2026-10-12.md` MED-1 for the new text): the 409 for an unreadable `prof_fav.bin` now sends the operator to "factory reset (scope kiln)", which does not delete that file. The test pins that wrong text.
- LOW-B (fixed in `bebc85025`): when the file is unreadable but a valid legacy NVS copy exists, `set()` still publishes an unsaved favorite before the refused save. LOW-1's second symptom ("the UI shows a favorite that is not saved") is therefore fixed only for the no-NVS case.

## Evidence

- **KilnFW target build** (`check_00_kilnfw_target_build.ps1`, at `3063da377`): PASS. `KilnCtrl.bin` produced.
- **Full -Fast suite** (`tools\run_all_checks.ps1 -Fast`): 175 passed, 17 SKIP-FAST, 5 failed. None of the 5 failures is in the reviewed code: `check_js_host_tests.ps1` and `check_page_js_tests.ps1` fail only on `test_zones_type_toggle.js` (the known failure); `check_gate_negative_test_table.ps1` reports that `GATE_NEGATIVE_TEST_EVIDENCE.md` has no row for `tools/check_lcd_sink_callgraph.ps1` / `test_check_lcd_sink_callgraph.ps1` (dev state outside this range); `check_land.ps1` hit its 900 s timeout under machine load; `check_main_baseline.ps1` fails its own baseline tests (tooling, outside this range).
- **Negtest** (`tools\negtest.ps1 -Preset kilnfw-host -Mutations <appendix> -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 3`, rerun of four with `-Parallel 2`): 21 mutations: the fixer's 15 (M1-M9, M1b; L2 is covered by A7, L1/L1b by A11/A2, I4 by A3) plus mine. 20 CAUGHT, 1 MISSED (A5, see INFO-G). Baseline passed in both runs. The first run (log dir `negtest_logs\20261012_033512_wk2w`) ended ERROR for reasons outside the mutations: I wrote this doc into the review tree during the run, and one worker hit a stale `index.lock`, so A1, A4, A7 and A10 never ran. Those four were rerun alone (`negtest_logs\20261012_052543_2hqa`): ALL_CAUGHT, real tree unchanged. Every CAUGHT was checked against its log: each has at least one `FAIL <test>.c:<line>` assertion in `test_cfg_fs_status.c` or `test_profiles_http.c`. Some logs also show BUILD FAILURES in unrelated executables (http_body_recv, http_form, update_fetch, ota_http, recovery_switch: `C1083 Permission denied` / `D8040` under load); none of those counts toward a verdict.

| Mutation | Verdict | Assertions that failed |
|---|---|---|
| `M1_conditions_never_rendered` | CAUGHT | test_cfg_fs_status.c:536, :204, :214 |
| `M1b_conditions_recapped_at_6` | CAUGHT | test_cfg_fs_status.c:536, :214 |
| `M2_bitmap_rebuild_condition_never_cleared` | CAUGHT | test_profiles_http.c:2153 |
| `M3_condition_clear_noop` | CAUGHT | test_cfg_fs_status.c:217, :218, :220; test_profiles_http.c:2144, :2153 |
| `M4_unresolved_zeroes_user_mask` | CAUGHT | test_profiles_http.c:2200, :2206, :2215, :2269 |
| `M5_unresolved_zeroes_builtin_mask` | CAUGHT | test_profiles_http.c:2201, :2205 |
| `M6_success_keeps_unresolved_flag` | CAUGHT | test_profiles_http.c:2251 |
| `M7_favorites_409_reverted_to_generic` | CAUGHT | test_profiles_http.c:2302, :2303, :2304, :2307 |
| `M8_unresolved_not_marked_degraded` | CAUGHT | test_profiles_http.c:2296, :2300-2306 |
| `M9_probe_error_condition_not_set` | CAUGHT | test_profiles_http.c:2131 |
| `A1_unreadable_file_treated_as_nothing_saved` | CAUGHT (rerun) | test_profiles_http.c:2266, :2269 |
| `A2_refusal_ignores_rev_unknown` | CAUGHT | test_profiles_http.c:2276, :2277 |
| `A3_builtin_not_found_keeps_stale_ram` | CAUGHT | test_profiles_http.c:2214 |
| `A4_info3_success_clear_dropped` | CAUGHT (rerun) | test_profiles_http.c:2144 |
| `A5_degraded_comma_by_index` | **MISSED** | none (INFO-G) |
| `A6_conditions_count_dropped` | CAUGHT | test_cfg_fs_status.c:535, :199, :204, :214, :220 |
| `A7_handler_buffer_back_to_5632` | CAUGHT (rerun) | test_cfg_fs_status.c:521 |
| `A8_set_never_retries_unresolved_load` | CAUGHT | test_profiles_http.c:2214, :2315, :2316, :2317 |
| `A9_success_load_publishes_nothing` | CAUGHT | test_profiles_http.c:2573, :2575, :2242 |
| `A10_publish_drops_builtin` | CAUGHT (rerun) | test_profiles_http.c:2540, :2575, :2201, :2205 |
| `A11_favorites_text_back_to_scope_kiln` | CAUGHT | test_profiles_http.c:2302, :2303 |

## Focus areas

### 1. `fav_publish()` spinlock

`profiles_favorites.c:56-62` copies a 16 B bitmap and a u32 under `s_fav_mux`. `profiles_favorites_masks()` (`:397-407`) copies both out under the same mux, and `profiles_favorites_is()` reads through `masks()`. No blocking call, NVS access, log statement or allocation runs under the lock, and no ISR uses it. Every writer runs either at boot or under `s_save_lock`, so the unlocked read of `s_fav_builtin` at `:266` (`uint32_t builtin = s_fav_builtin;`) cannot race a writer. This is sound.

Two notes (INFO-A, INFO-B below): the LCD picker still mixes publishes across ids, and at boot "keep the current RAM value" means zero.

### 2. Unresolved favorites state

- **Wedge and heal.** When unresolved, `set()` (`:321-327`) re-runs `favorites_load()` on every request. `pref_cfg_fs_resolve()` clears both the rev-unknown entry and the degraded entry (`unknown_set(path, false)`, `pref_cfg_fs.c:486`) once the file reads cleanly. A transient read failure therefore heals on the next favorite request with no reboot. A cfg format (`POST /api/cfgfs/format_confirm`, no reboot) leaves the file absent, and the next request also reads it as absent (resolved, no zero-mask problem, since an absent file is a clean read). This is sound.
- **Factory reset.** Scope `profiles` and scope `all` delete `prof_fav.bin` (`profiles_scope_cfg_files.c:36`) and reboot. Scope `kiln` sets only `delete_kiln_cfg_files` (`factory_reset.c:150`) and leaves the file alone. That is why LOW-A matters.
- **Profile delete while unresolved.** `profiles_http.c:2242` calls `(void)profiles_favorites_set(id, false)` best effort. While unresolved this is refused, so the favorite bit stays in the unreadable file. When the file later heals, that bit comes back on a now-empty or reused slot id (INFO-E). The effect predates these commits.
- **Reset-one-side class.** `s_fav_user_unresolved`, the rev-unknown entry and the degraded entry are three copies of "this store is not trusted". Today `favorites_load()` sets or clears all three on every path, and `pref_cfg_fs_resolve()` owns the last two. I found no path that clears one and not the others. One asymmetry remains: the resolved-from-NVS case clears `s_fav_user_unresolved` while the rev-unknown entry stays set (LOW-B). That is the intended "adopt NVS, refuse writes" design, but `set()` does not consult the second copy before it mutates RAM.

### 3. Heap scratch for the 6656 B buffer

`diagnostics_http.c:1481-1485` holds `zones_cfg_t raw`, the window status and `json[6656]` in one `cfgfs_status_scratch_t`. It is allocated with `persist_scratch_alloc()` (`:1541`), which tries PSRAM first and falls back to internal `malloc`. A NULL sends a 500 "out of memory" before any output. A build failure (`:1763-1769`) sends a 500 "status build failed" and frees, so no partial JSON goes out. The buffer never touches the httpd stack.

The 8192 B internal floor is reached only if PSRAM is exhausted and the fallback takes about 7.5 KB of internal heap for the duration of one GET. That fallback is pre-existing and shared by every `persist_scratch_alloc()` caller.

The worst case is honest:

- The handler adds exactly 20 dual-write rows (`diagnostics_http.c:1577-1742`).
- The test (`test_cfg_fs_status.c:460-535`) uses those same 20 names at `UINT32_MAX` revs, 18 root files plus a `profiles/` subdirectory at six-digit sizes, 9 degraded names (6 are listed) and 8 conditions (the table cap), all at the 47-char registry maximum.
- It then adds the gap to the longest `esp_err` name and asserts 400 B of headroom.

The comment at `diagnostics_http.c:1469-1472` (">= 400 B of margin in 4608 B") and the one at `test_cfg_fs_status.c:458` ("5632 B") are stale (INFO-D).

### 4. INFO-4 NOT_FOUND

`favorites_read_builtin_only()` (`:211-226`) sets `*builtin = 0` only on `HAL_NOT_FOUND`, which `hal_kv_esp.c:142` maps from `ESP_ERR_NVS_NOT_FOUND` alone. A missing partition, a failed open and other read errors leave the RAM value. While unresolved, `set()` refuses, so this 0 never reaches flash. A wrong NOT_FOUND could at worst hide built-in favorites in RAM until the next clean load. This is sound.

## Findings

### LOW-A: an unreadable `prof_fav.bin` gets recovery advice that does not work (fixed in `bebc85025`)

- **Where:**
  - `firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h:96-98`: when `pref_cfg_fs_rev_unknown(store)`, `cfg_fs_http_persist_failed_favorites()` falls through to the generic `cfg_fs_http_persist_failed_for()`.
  - `cfg_fs_refusal_http.h:40` (`CFG_FS_STORE_UNREADABLE_REASON`): the generic text says to clear the store "with a cfg format ... or a factory reset (scope kiln)".
  - `test/test_profiles_http.c:2277` asserts the 409 does NOT mention `scope=profiles`.
- **Scenario:**
  1. `prof_fav.bin` is unreadable at boot.
  2. The operator taps a favorite and gets the 409.
  3. The 409 offers cfg format or factory reset scope kiln.
  4. The operator picks scope kiln, which erases kiln settings and reboots. Kiln scope does not delete `prof_fav.bin` (`factory_reset.c:150`; only scope profiles/all run `profiles_scope_cfg_files_delete()`, `profiles_scope_cfg_files.c:36`).
  5. The favorites store is still unreadable after the reboot, and the kiln configuration was wiped for nothing.

  The prior review's LOW-1 asked for text naming cfg format or scope profiles/all. The fix took the generic text instead, and the test now forbids the correct remedy.
- **Fix:**
  - Add a third text, e.g. `CFG_FS_FAVORITES_FILE_UNREADABLE_REASON`: "the favorites file could not be read ...; retry (each request re-reads it, a reboot also retries); if it persists, a cfg format (POST /api/cfgfs/format_confirm force_healthy=1) or factory_reset scope=profiles/scope=all clears it; scope kiln does not."
  - Use it in the rev-unknown branch of `cfg_fs_http_persist_failed_favorites()`.
  - Flip the test at `:2277` to require `scope=profiles` and forbid `scope kiln`.

### LOW-B: an unsaved favorite is still shown when the file is unreadable but a legacy NVS copy exists (fixed in `bebc85025`)

- **Where:** `firmware/KilnFW/App/drivers/persist/profiles_favorites.c:345` (`fav_publish(&cur_user, cur_builtin);` runs before `pref_cfg_fs_commit()` at `:359`). The other half is `pref_cfg_fs.c:473-480`: when the file is unreadable and the NVS copy is valid, the NVS copy is adopted, the call returns `have=true`, and rev-unknown stays set.
- **Scenario:**
  1. A board still has the legacy `prof_favusr`/`prof_favbi` keys, and `prof_fav.bin` is unreadable at boot (I/O error or allocation failure).
  2. `favorites_load()` takes the resolved branch, so `s_fav_user_unresolved = false` and `set()` does no reload.
  3. The operator favorites profile 3. `set()` publishes the new mask, then `pref_cfg_fs_save()` refuses on rev-unknown (`pref_cfg_fs.c:373`), and the handler sends a 409.
  4. The LCD picker and `GET /api/profiles` show profile 3 as a favorite anyway. It disappears on reboot. That is the second symptom of REVIEW_PROFILESFX2 LOW-1, now fixed only for the no-NVS case.
  5. In addition, `s_fav_dirty` is set, so every later no-op `set()` retries the refused write.
- **Fix:** before mutating, refuse early when `pref_cfg_fs_rev_unknown(PROFILES_FAVORITES_FILE_PATH)` (the same 409 path), or keep the pre-mutation masks and re-publish them when `pref_cfg_fs_commit()` fails with `ESP_ERR_INVALID_STATE`. Add a test with a valid NVS copy and an unreadable file: assert that `profiles_favorites_is(id)` stays false after the refused set.

### INFO-A: the LCD picker can still mix publishes across ids

`ui_page_profile_picker.c:135` and `:314` call `profiles_favorites_is()` once per id, so one picker render can show part of an old mask and part of a new one. No single id is torn. `profiles_catalog_http.c:605` takes one `masks()` snapshot and is consistent. Optional fix: take one `profiles_favorites_masks()` snapshot per render.

### INFO-B: at boot, "keep the current RAM value" means zero

On the unresolved path (`profiles_favorites.c:266-270`), `keep_user` is the current published mask. At boot that is the zero-initialised static, so a boot with an unreadable file publishes an empty user mask (the builtin mask comes from its own key). "No zero mask is published" holds only for runtime reloads. This is correct behaviour, since no better value exists, but the comment at `:230-232` reads stronger than that.

### INFO-C: `start()` now reports unresolved favorites as a startup fault

`favorites_load()` returns `ESP_ERR_INVALID_STATE` for "file unreadable, no NVS copy" (`:271`), so `App/main_network_http.c:417` now logs a warning and calls `startup_fault_note(STARTUP_FAULT_SETTINGS_STORE)` for that case. That is arguably right, but it is a new visible behaviour that the Fix status table does not mention.

### INFO-D: stale buffer-size comments

`diagnostics_http.c:1469-1472` still says ">= 400 B of margin in 4608 B", and `test_cfg_fs_status.c:458` says "5632 B". Both should cite `CFG_FS_STATUS_HANDLER_JSON_BUF` (6656 B).

### INFO-E: a favorite on a deleted profile can come back

A profile delete while unresolved leaves the favorite bit in the file (`profiles_http.c:2242`, best effort). When the file later heals, the bit returns on the deleted, or reused, slot id. This is pre-existing. Optional fix: on the first successful load after an unresolved period, mask the user bitmap against the used-slot bitmap.

### INFO-F: degraded list still capped at 6 names

`cfg_fs_status.c:191` lists at most 6 of up to `CFG_FS_DEGRADED_MAX` (40) degraded names; `degraded_count` is exact. That is the documented behaviour and the buffer is sized for it. The conditions list is now uncapped, at its table cap of 8.

### INFO-G: the INFO-2 degraded-list comma fix has no test

Mutation A5 reverts the degraded loop in `cfg_fs_status.c` from the `emitted` flag back to `i ? "," : ""` and every test still passes. The old form emits a leading comma (invalid JSON) when `cfg_fs_degraded_name(0, ...)` returns false and a later index succeeds, for example when an entry is cleared between `cfg_fs_degraded_count()` and the name read (`cfg_fs_status.c:189-197`). No test makes the first name read fail. Fix: add a test seam (or a race stub) that fails `cfg_fs_degraded_name()` for index 0 only, and assert the `degraded` array has no leading comma.

## Appendix: negtest mutations (JSON)

```json
[
 {
  "name": "M1_conditions_never_rendered",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs_status.c",
  "find": "for (int i = 0; i < cn; i++) { /* INFO-1",
  "replace": "for (int i = 0; i < 0; i++) { /* INFO-1",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M1b_conditions_recapped_at_6",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs_status.c",
  "find": "for (int i = 0; i < cn; i++) { /* INFO-1",
  "replace": "for (int i = 0; i < cn && i < 6; i++) { /* INFO-1",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M2_bitmap_rebuild_condition_never_cleared",
  "file": "firmware/KilnFW/App/drivers/http/profiles_http.c",
  "find": "    cfg_fs_condition_set(\"profiles_used_bitmap\", false);\n    *out = rebuilt;",
  "replace": "    (void)0;\n    *out = rebuilt;",
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
  "find": "        s_fav_user_unresolved = true;\n        cfg_fs_degraded_set",
  "replace": "        s_fav_user_unresolved = true; profiles_slot_bitmap_from_u32(&s_fav_user, 0);\n        cfg_fs_degraded_set",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "M5_unresolved_zeroes_builtin_mask",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "        fav_publish(&keep_user, builtin);",
  "replace": "        fav_publish(&keep_user, 0); (void)builtin;",
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
  "find": "            cfg_fs_condition_set(\"profiles_used_bitmap\", true); /* LOW-3",
  "replace": "            (void)0; /* LOW-3",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A1_unreadable_file_treated_as_nothing_saved",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "if (nerr == HAL_OK && !file_unreadable) {",
  "replace": "if (nerr == HAL_OK) { (void)file_unreadable;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A2_refusal_ignores_rev_unknown",
  "file": "firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h",
  "find": "err == ESP_ERR_INVALID_STATE &&\n        !pref_cfg_fs_rev_unknown(store)) {",
  "replace": "err == ESP_ERR_INVALID_STATE) {",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A3_builtin_not_found_keeps_stale_ram",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "        *builtin = 0; /* INFO-4",
  "replace": "        (void)0; /* INFO-4",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A4_info3_success_clear_dropped",
  "file": "firmware/KilnFW/App/drivers/http/profiles_http.c",
  "find": "        cfg_fs_condition_set(\"profiles_used_bitmap\", false); /* INFO-3",
  "replace": "        (void)0; /* INFO-3",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A5_degraded_comma_by_index",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs_status.c",
  "find": "APPEND(\"%s\\\"%s\\\"\", emitted ? \",\" : \"\", dname);",
  "replace": "APPEND(\"%s\\\"%s\\\"\", i ? \",\" : \"\", dname);",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A6_conditions_count_dropped",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs_status.c",
  "find": "APPEND(\",\\\"conditions_count\\\":%d,\\\"conditions\\\":[\", cn);",
  "replace": "APPEND(\",\\\"conditions\\\":[\");",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A7_handler_buffer_back_to_5632",
  "file": "firmware/KilnFW/App/drivers/persist/cfg_fs_status.h",
  "find": "#define CFG_FS_STATUS_HANDLER_JSON_BUF 6656u",
  "replace": "#define CFG_FS_STATUS_HANDLER_JSON_BUF 5632u",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A8_set_never_retries_unresolved_load",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "        (void)favorites_load(); /* MED-1",
  "replace": "        /* MED-1",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A9_success_load_publishes_nothing",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "    fav_publish(&resolved.user, resolved.builtin);\n    s_fav_rev = rev;",
  "replace": "    (void)0;\n    s_fav_rev = rev;",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A10_publish_drops_builtin",
  "file": "firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find": "    s_fav_builtin = builtin;\n    portEXIT_CRITICAL(&s_fav_mux);",
  "replace": "    (void)builtin;\n    portEXIT_CRITICAL(&s_fav_mux);",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 },
 {
  "name": "A11_favorites_text_back_to_scope_kiln",
  "file": "firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h",
  "find": "only factory_reset scope=profiles (or scope=all) does",
  "replace": "only factory_reset scope=kiln does",
  "expect": "(?m)^\\s+FAIL |FAIL .*\\.c:\\d+|RUN FAILURES|BUILD FAILURES"
 }
]
```
