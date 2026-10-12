# Review: profilesfx9 (2dc3c0eb1, 53bab3762, 4bf55c4b1) -- REVIEW_PROFILESFX8 MED-1, LOW-1, LOW-2

Adversarial review of three origin/dev commits:

- `2dc3c0eb1`: adds `CFG_FS_PROFILES_UNREADABLE_REASON` and the `cfg_fs_http_persist_failed_*_profiles` helpers
  (`firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h:46-51,71-128`), routes the profile save, delete and
  hidden.json routes to them (`profiles_edit_http.c:782,874,936,957`), rewords the generic text's tail (`:40`), adds
  the `?force_healthy=1, then reboot` spelling to the `zones_config_undecided` 409 (`zones_http_post.c:71-77`), and
  updates `unreadable_store_refusal()` (`tools/PcTools/src/kilnctrl/zones_http_client.py:166-193`).
- `53bab3762`: a test that calls the generic helper directly (`test_profiles_http.c:6194-6203`).
- `4bf55c4b1`: docs only. It marks fx8 MED-1, LOW-1 and LOW-2 fixed.

**Result: 0 HIGH, 1 MED, 3 LOW, 3 INFO.**

- Every remedy in the three texts is true against the firmware.
- The routes this commit touched choose the right text.
- No escaping or truncation problem was found.
- 7 of 10 mutants were CAUGHT by assertion failures. The 3 MISSED mutants are untested routes (LOW-2).
- The MED is older than this commit, but it is inside the requested routing scope: `POST /api/profile/import` on a
  rev-unknown slot answers 200 `{"ok":true}` and persists nothing. That route sends no refusal at all, so it never
  reaches the new text.

## What was checked

### (1) Are the remedies true?

- **"factory_reset scope=profiles (or scope=all) clears it and ERASES every user profile": true.**
  - The scope exists: `kProfilesOnly = {profiles_nvs}` (`factory_reset.c:145`), listed in `kScopes`
    (`:148-152`).
  - It erases the `profiles_nvs` partition.
  - It deletes every profile slot file, the firing-stats file, the favorites file and the live files
    (`profiles_scope_cfg_files.c:28-38`, called from `factory_reset.c:442-445`).
  - It discards `hidden.json` (`profiles_builtin_restore_all()` / `profiles_builtin_discard_file()`,
    `factory_reset.c:244-256`, `profiles_builtin.c:340-350`).
  - It reboots (`reboot_task`, `factory_reset.c:164-170`).
  - On the next boot the refusal is gone:
    - `nvs_read_rev_floors()` returns REV_KNOWN on HAL_NOT_FOUND, which is what an erased partition gives
      (`profiles_http.c:1102-1116`).
    - No slot file is left to fail its resolve.
    - So `s_profile_rev_unknown[]` (`:79`) is not set at `:1146`, `:1177`, `:1186`, `:1378` or `:1391`, and
      `pref_cfg_fs.c:302` does not mark `hidden.json`.
- **"a cfg format (POST /api/cfgfs/format_confirm?force_healthy=1, then reboot)": correct spelling.**
  - The handler is registered at `cfg_fs_format_http.c:134`.
  - `format_confirm_post_handler()` (`:44`) reads `force_healthy` from the URI query only (`:72`).
  - It reads it through `cfg_fs_confirm_uri_force_healthy()` (`cfg_fs_format_gate.c:276-301`), where only the exact
    value `1` counts.
  - The format job does not re-run any store's resolve (`cfg_fs_mount.c:561-592`), so "then reboot" is needed.
- **"clears it only when the profile file itself is unreadable": true.**
  - The format wipes the slot files and `hidden.json`.
  - The rev floors live in `profiles_nvs`, which a cfg format never touches. An unreadable floor blob, or an
    IO_ERR on its read, survives the format and sets `:1177`/`:1378` again on the next boot.
- **"reboot the controller to retry": true, with "if it persists" correctly hedged.**
  - A reboot re-runs every resolve. It clears transient causes: OOM at `:1146`, and a junk rev that the REV_JUNK
    repair fixes (`:1182`, `:1387`).
  - It does not clear a persistent IO_ERR.
- **"a factory reset with scope kiln does not clear it": true.**
  - `kKilnOnly = {kiln_nvs}` (`factory_reset.c:144`).
  - Its file list, `kKilnScopeFiles` (`kiln_scope_cfg_files.c:25-40`), names no profile file and no `hidden.json`.
  - `format_cfg_fs` is true only for scope all (`factory_reset.c:413`).
- **The generic text now ends "...the zones config and preferences; alternatively use a factory reset (scope
  kiln)" (`cfg_fs_refusal_http.h:40`): true for every remaining caller.**
  - The callers that can send it are:
    - `diagnostics_http.c:1084` (ramp assist)
    - `iter_tune_http.c:260`
    - `settings_http.c:126` (tz) and `:248` (display power)
    - `setup_progress_http.c:207`
    - `zones_http_post.c:896` (zones.json)
    - `update_settings_http.c:106`
    - `dashboard_settings_http.c:83` (unit pref)
  - Every one of those stores is in `kKilnScopeFiles`, and its NVS side is `kiln_nvs`.
  - `profiles_edit_http.c:872` and `:876` also use the generic helper, but they pass `store_refused=false` or call
    `cfg_fs_http_persist_failed()`. Both answer 500 and never send this 409 text.
- **The zones undecided 409 (`zones_http_post.c:71-77`): true.**
  - `s_zones_cfg_undecided` (`zones_config_store.c:102`) is set at `:621`, `:648` and `:712`, and cleared only by a
    clean load (`:612-613`).
  - Scope kiln deletes `zones.json` and `zones.json.bad`, and a cfg format wipes them. Either way the next boot loads
    defaults cleanly.
- **The Python client (`zones_http_client.py:166-193`): true, and harmless where it is used.**
  - Its only callers are the four zones writers in `mcp_server_control.py` (`:825`, `:1005`, `:1392`, `:1578`). None
    of them can receive a profile refusal.
  - The firmware's own `reason` is passed through verbatim in `detail`.

### (2) Routing coverage

**Profile routes:**
- **POST /api/profile save:** correct. `:782` uses `_state_profiles(req, s_profile_rev_unknown[target_id], err, -1)`.
  `nvs_save_slot_locked()` also returns ESP_ERR_INVALID_STATE for a PSRAM stack or a refused save-lock reset
  (`profiles_http.c:1484`), but those take the 409 only when the slot is also rev-unknown, and then the profile text
  is right anyway.
- **Delete:** correct. `PROFILES_DELETE_REV_UNKNOWN` (`profiles_http.c:2228-2230`) goes to `:874`, the profile
  helper.
- **Builtin hide and restore:** correct. `:936` and `:957` use the profile helper keyed on `profiles/hidden.json`.
  That is the same string the store registers (`profiles_builtin.c:51`, `pref_cfg_fs.c:302`). See INFO-2 for the
  wording.
- **Favorites:** keep their own text (`:1020`). Not changed. Correct.
- **Gaps:** import, the LCD builder, backup import, the UART bridge, live decide and zone-to-aux conversion. See
  MED-1, LOW-1 and INFO-1.

**Zones routes:**
- `POST /api/zones` sends the undecided 409 at `zones_http_post.c:700` and `:826`, and the generic `zones.json` text
  at `:896`.
- Other zones writers map a refused save to a 500 with no remedy text (LOW-3).

### (3) Escaping, buffers, truncation, consumers

- **Escaping:** none of the three texts contains `"` or `\` (checked by script). Each is pasted as a string literal
  into the JSON skeleton of `CFG_FS_SEND_409_REASON` (`cfg_fs_refusal_http.h:71-80`).
- **Buffers in firmware:** none. Firmware never copies the text into a buffer; `httpd_resp_sendstr()` gets the
  literal, so nothing can truncate it.
- **Body sizes:**

  | Body | Size |
  |---|---|
  | Profile reason | 547 B |
  | Profile 409 body | 606 B; 622 B with `adopted:false` (that path is not used: every profile caller passes `adopted=-1`) |
  | Generic reason | 422 B |
  | Generic 409 body | 481 B; 497 B with `adopted:false` |

- **Test captures:** the profile body is captured only by `test_profiles_http.c` (2048 B, `:352`). The 512 B
  captures in `test_iter_tune_http.c:288`, `test_kiln_cfg_http.c:309` and `test_adaptive_tune_http_gate.c:122` can
  only see the generic body, which now leaves 15 B of headroom (INFO-3).
- **Consumers:** none. No web page (`*.html`/`*.js`) and no LCD code reads `store_unreadable_at_boot` or
  `zones_config_undecided`; a grep finds them only in the two firmware files that send them. The LCD never renders
  these texts.

## Findings

### MED-1: POST /api/profile/import on a rev-unknown slot answers 200 `{"ok":true}` and persists nothing (pre-existing)

**Where:** `import_post_handler()` calls `profiles_http_save()` (`profiles_export_http.c:465`). That function passes
`out_persisted = NULL` (`profiles_http.c:1853-1857`).

**What happens:**
- In `profiles_http_save_impl()`, `nvs_save_slot_locked()` refuses the slot (`:2056`; rev-unknown gives
  ESP_ERR_INVALID_STATE at `:1484`).
- The failure path only logs "profile applied live but will not survive a reboot" (`:2088-2095`) and returns true
  (`:2101`).
- The handler then sends `{"ok":true,"id":N,"warning_count":W}` (`:481-484`).
- For `?id=N` naming an existing slot, the RAM copy is replaced while the stored file is not. The next firing uses
  the imported profile, and a reboot silently reverts to the old one.
- The operator gets no refusal, so neither the new profile text nor any other remedy is ever shown.

**Same NULL-persisted path, all reporting success:**
- the LCD builder save (`ui_page_profile_builder_review.c:124`, `profiles_http_save_expect_rev()`, `:1870-1874`)
- backup import (`backup_import.c:3033`)
- the UART bridge SAVE (`uart_bridge_ext_control.c:506`)

**Why it is in scope:** it is older than profilesfx9, but the task asks for every profile route that can hit an
unreadable store.

**Fix:**
- Pass a `persisted` flag in the import handler. On false, answer through
  `cfg_fs_http_persist_failed_state_profiles(req, <slot rev-unknown>, <err>, -1)`.
- That needs either a new accessor for `s_profile_rev_unknown[id]` or a save variant that returns the `esp_err_t`.
- Give the LCD and UART callers the same treatment, at least as a refusal string.

### LOW-1: live decide save_as/overwrite on a rev-unknown slot says "retry" (pre-existing)

**Where:** `profiles_live_http.c:714-719` and `:755-757`.

**What happens:**
- Both answer 500 "storage save failed; the edit is kept, retry". Retrying cannot work: the rev-unknown state is
  sticky for the whole boot.
- The 409 `store_unreadable_at_boot` and its remedy are never sent.
- For overwrite, the origin slot's RAM copy has already been replaced, so RAM and storage disagree until a reboot.

**Fix:** carry the save's `esp_err_t` out of `profiles_http_save_ex()` and map ESP_ERR_INVALID_STATE on a rev-unknown
slot to the profile 409.

### LOW-2: no test pins the profile text on the save, hide or restore routes

**What is tested:** only the delete route (`test_profiles_http.c:6179-6192`) and the impl flag are asserted.

**Mutants missed:**
- M4: routing the save at `:782` back to the generic helper
- M5: routing `:936` back to the generic helper
- M6: routing `:957` back to the generic helper

**Why it matters:** each of those mutants restores exactly the fx8 MED-1 bug on that route. No test drives
`builtin_hide_post_handler`/`builtin_restore_post_handler` with a degraded `hidden.json`.

**Fix:** add three short tests, one per route:
- mark the slot rev-unknown (save) or the path degraded (hide/restore)
- POST
- assert 409 `store_unreadable_at_boot` with `factory_reset scope=profiles` and `does not clear it"}`

### LOW-3: other zones writers send a 500 with no remedy when the zones config is undecided (pre-existing)

**Where:**
- `zones_http_pid.c:187-191`: any `zones_set_result_t` other than OK or BUSY_RUNNING, including SAVE_FAILED from
  the undecided refusal at `zones_config_store.c:911`, gives 500 "failed to apply/persist PID gains".
- `iter_tune_http.c:221-230`: 500 "zones_config_set_pid refused".

**What is missing:** neither names `zones_config_undecided`, so `unreadable_store_refusal()` cannot recognise them
and the operator gets no recovery route.

**Fix:** check `zones_config_is_undecided()` (`zones_config_store.c:121`) on failure and send
`zones_post_send_undecided_409`'s body. That body would need to move to a shared header.

### INFO-1: zone-to-aux conversion fails with no remedy when a profile slot is rev-unknown

**Where:** the plan step (`profiles_http.c:2607-2666`) does not pre-check `s_profile_rev_unknown[]`. A slot that
needs rewriting fails at commit (`:2714-2800`) as "persisting the rewritten profile failed".

**Severity:** the conversion is all-or-nothing and rolls back, so nothing is lost. A pre-check with the profile
remedy would be clearer.

### INFO-2: the profile text says "a stored profile" when the refused store is hidden.json

**What happens:** on `:936`/`:957` the refused store is the hidden-builtin list, not a profile. The remedies stated
are still true for it:
- scope=profiles discards `hidden.json`
- a cfg format wipes it
- scope kiln does not touch it

**Possible fix:** "a stored profile or the hidden built-in list" would be exact.

### INFO-3: the generic 409 now leaves 15 B in the 512 B host-test captures

**What happens:** the generic body with `adopted:false` is 497 B. It fits in the 512 B captures at
`test_iter_tune_http.c:288`, `test_kiln_cfg_http.c:309` and `test_adaptive_tune_http_gate.c:122`. None of those
tests asserts the tail.

**Risk:** a future wording change could silently truncate there. Raising those captures to 1024 B would remove the
trap.

## Negtest

**How it was run:**
- `tools\negtest.ps1 -Command "powershell ... build_host_tests.ps1 -OutDir {OUT} -Only profiles_http|zones_http"`
  with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
- Base `4bf55c4b1`. Baseline PASS. Real tree unchanged.
- No mutant hit BUILD FAILURES: every CAUGHT below is a compiled mutant with a failing `TEST_CHECK`.
- Python: `-Preset pytest -PresetArg tests/test_zones_http_client.py`. Baseline PASS. Real tree unchanged.

| Mutation | Verdict | Decisive assertion(s) |
|---|---|---|
| M1 profile text: tail "a factory reset with scope kiln does not clear it" -> "alternatively use a factory reset (scope kiln)" | CAUGHT | `test_profiles_http.c:6192` |
| M2 impl: `if (profiles)` -> `if (!profiles)` | CAUGHT | `:6191`, `:6192`, `:6201`, `:6202` |
| M3 delete `:874` -> generic `cfg_fs_http_persist_failed_state` | CAUGHT | `:6191`, `:6192` |
| M4 save `:782` -> generic helper | MISSED | none (LOW-2) |
| M5 hide `:936` -> `cfg_fs_http_persist_failed_for` | MISSED | none (LOW-2) |
| M6 restore `:957` -> `cfg_fs_http_persist_failed_for` | MISSED | none (LOW-2) |
| M7 generic text: "; alternatively use a factory reset" -> ", or a factory reset" | CAUGHT | `:6201` |
| M8 zones 409: drop ", then reboot" | CAUGHT | `test_zones_http.c:2203` |
| P1 client: drop "scope kiln does not clear a profile refusal." | CAUGHT | `test_zones_http_client.py::UnreadableStoreRefusalTest::test_store_unreadable` |
| P2 client: "factory_reset scope=profiles (or scope=all)" -> "factory_reset (scope=all)" | CAUGHT | same |

Logs: `%TEMP%\negtest_logs\20261012_091334_jp08` (firmware), `%TEMP%\negtest_logs\20261012_091345_rlb3` (pytest).

## Fix status (PROFILESFX10)

- MED-1 FIXED. `profiles_http_save_impl` refuses a rev-unknown target slot before any RAM assignment, returning
  `false` with `err_msg == PROFILES_SAVE_ERR_STORE_UNREADABLE` (`profiles_store.h`). Every caller sees the failure:
  the import route answers 409 with the profile remedy; backup import reports `rejected at commit: <that text>`; the
  LCD builder and the UART bridge show the text. Tests: `test_profiles_http.c`
  `test_save_refuses_rev_unknown_slot_without_mutating_ram` (save, save_expect_rev, save_ex; RAM and used bit
  unchanged), `test_profile_export_import.c` `test_store_unreadable_save_refusal_is_409_with_remedy`. Two older tests
  that relied on a rev-unknown save landing in RAM and rolling back were updated to the refuse-up-front behaviour.
- LOW-1 FIXED. `LIVE_DECIDE_STORE_UNREADABLE` (409, profile remedy) for save_as and overwrite; the pending edit is
  kept. Test: `test_profiles_live_http.c` `test_decide_store_unreadable_is_409_with_remedy`.
- LOW-2 FIXED. `test_profile_routes_use_profile_409_text` pins the profile text on save, hide and restore (M4-M6).
- LOW-3 FIXED. `zones_http_pid.c` and `iter_tune_http.c` answer the zones-undecided 409 (body moved to
  `cfg_fs_http_send_zones_undecided_409` in `cfg_fs_refusal_http.h`) when `zones_config_is_undecided()`. Tests in
  `test_zones_http.c` and `test_iter_tune_http.c`.
- INFO-1 not fixed (rolls back safely; cheap-only scope).
- INFO-2 FIXED (wording "a stored profile (or the hidden built-in list)").
- INFO-3 FIXED (captures raised to 1024 B in the three tests).
