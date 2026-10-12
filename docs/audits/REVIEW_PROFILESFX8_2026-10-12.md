# Review: profilesfx8 (849c2bf36) -- REVIEW_PROFILESFX7 LOW-1/2/3, INFO-5

Adversarial review of origin/dev `849c2bf36`: new wording for `CFG_FS_STORE_UNREADABLE_REASON` and
`CFG_FS_FAVORITES_FILE_UNREADABLE_REASON` (`firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h:40,52-53`), the
`test_profiles_http.c` stub buffer raised to 2048 B (`:352`), and new assertions (`:2283-2286`, `:6188-6191`).

**Result: 0 HIGH, 1 MED, 2 LOW, 3 INFO.** The fx7 fixes are correct as far as they go, and every new assertion is
non-vacuous (8/8 mutants CAUGHT by assertion failures). The MED is old advice that this commit rewrote and then pinned
in a test: the generic text is also served on profile routes, where "factory reset (scope kiln)" cannot clear the
refusal.

## What was checked

### (1) Are the remedies accurate?

- **Route and parameter: correct.** `format_confirm_post_handler()` (`cfg_fs_format_http.c:72`) reads only the URI
  query through `cfg_fs_confirm_uri_force_healthy()` (`cfg_fs_format_gate.c:276`). Only the exact `?force_healthy=1`
  counts. Both texts now use that spelling.
- **"then reboot": correct and needed.** `cfg_fs_confirm_format_job_run()` (`cfg_fs_mount.c:561-592`) formats,
  re-registers and remounts. It does not re-run any store's resolve, so `pref_cfg_fs.c`'s rev-unknown entries, the
  degraded table, `zones.json`'s degraded flag (`zones_config_store.c:108`) and `s_profile_rev_unknown[]` all last
  until the next boot.
- **"ERASES ALL cfg-stored settings: every user profile, the zones config and preferences": accurate.** Profiles,
  `zones.json`, prefs and `prof_fav.bin` all live on the `cfg` LittleFS partition, which `esp_littlefs_format()`
  wipes. See INFO-1 for what the list leaves out.
- **"scope=all ... also wipes Wi-Fi and kiln NVS": accurate.** `kAll` = wifi_nvs, kiln_nvs, profiles_nvs, and
  `format_cfg_fs` is true only for "all" (`factory_reset.c:146,152,413`).
- **"factory reset (scope kiln)" in the generic text: wrong for profile routes.** See MED-1.

### (2) Do the strings fit their buffers?

- Firmware never copies these strings. Each is part of a string literal passed straight to `httpd_resp_sendstr()`
  (`cfg_fs_refusal_http.h:71-75,106-108`), so nothing can truncate them. `PROFILES_SAVE_ERR_MSG_LEN` (192,
  `profiles_store.h:59`) is used only by `ui_page_profile_builder_review.c:93` for profile-builder errors, never for
  these texts. The LCD never renders them, and no web page reads `reason` for a `store_unreadable_at_boot`.
- Body sizes: favorites FILE 547 B; generic 466 B, or 482 B with the `adopted` field. Other host-test harnesses that
  can receive the generic 409 have a 512 B capture (`test_iter_tune_http.c:288`, `test_kiln_cfg_http.c:309`,
  `test_adaptive_tune_http_gate.c:122`), which leaves 29 B of headroom. None of them asserts the tail. Harnesses that
  do assert the tail are large enough: `test_zones_http.c:402` (8192 B) and `stubs_update_fetch/fake_support.h:76`
  (16384 B). No silent truncation was found.

### (3) Are the new assertions non-vacuous?

Yes. See the negtest table below. Every CAUGHT row is a mutant that compiled (no `BUILD FAILURES` and no compiler
errors in any log) and whose run failed on the listed assertion.

### (4) Callers or checks broken by the new text

None. `git grep` over the tree (docs excluded) finds no consumer of the old wording. `test_update_http_refusals.c:150`
and `test_zones_http.c:2199` assert only `force_healthy` and `scope kiln`, and the new text still contains both.
Earlier audit docs quote the old text as history only.

## Findings

### MED-1: the generic text tells profile-route callers that "a factory reset (scope kiln)" clears the refusal; it cannot, and the new test pins it **[FIXED in profilesfx9: profile routes use CFG_FS_PROFILES_UNREADABLE_REASON, generic list split, zones/PcTools text completed]**

`CFG_FS_STORE_UNREADABLE_REASON` is not only for kiln-scope stores. These profile paths also send it:

- `profiles_http.c:2228` (a delete on a rev-unknown slot), which is what `test_delete_rev_unknown_is_409` drives;
- `profiles_edit_http.c:782` and `:874` (profile saves);
- `profiles_edit_http.c:936` and `:957` (`profiles/hidden.json`).

On those paths the remedies fail as follows:

- **scope kiln never helps.** It erases only `kiln_nvs` (`factory_reset.c:144,150`) and deletes the
  `kiln_scope_cfg_files.c:25-40` mirrors. Neither `profiles_nvs`, the profile files nor `hidden.json` is touched.
  An operator who follows the advice wipes the zones config, the kiln config slots, the relay names, the tuning
  stores and the crash report, and the profile 409 stays.
- **cfg format helps in only one case.** A slot is rev-unknown either because its file resolve failed
  (`profiles_http.c:1164,1391`) or because the NVS rev-floor blob in `profiles_nvs` could not be read
  (`profiles_http.c:1176-1177,1378`). A format clears only the first case. In the second, the format erases every
  profile and the refusal still comes back after the reboot. Only factory_reset scope=profiles (or all) clears it,
  and the text never names that scope.

This text predates the commit. But the commit rewrote it to fix fx7 LOW-3, and it adds
`TEST_CHECK(strstr(s_last_sendstr, "(scope kiln)\"}") != NULL, ...)` (`test_profiles_http.c:6191`) on the profile
delete body, which locks the wrong remedy into the test.

**Fix:** give the profile paths their own text, as was done for favorites (FIRELOWFX3 LOW-1 / PROFILESFX4 LOW-A).
For example: "...; if it persists after a reboot, factory_reset scope=profiles clears it (and ERASES every user
profile); a cfg format (…?force_healthy=1, then reboot) clears it only when the profile file itself is unreadable,
and ERASES ALL cfg-stored settings; scope kiln does not clear it". Also change `:6191` to assert the end of the new
text.

### LOW-1: the generic text's colon list swallows the second remedy **[FIXED in profilesfx9: profile routes use CFG_FS_PROFILES_UNREADABLE_REASON, generic list split, zones/PcTools text completed]**

"...which ERASES ALL cfg-stored settings: every user profile, the zones config and preferences, or a factory reset
(scope kiln)" (`cfg_fs_refusal_http.h:40`). Read literally, "a factory reset" is one of the things the format
erases. Use a semicolon, or close the list and repeat the subject, e.g. "...and preferences; or use a factory
reset (scope kiln)". This is moot for profile routes once MED-1 is fixed, but still applies to the kiln-scope
stores.

### LOW-2: two sibling remedy texts still have the fx7 LOW-3 defects **[FIXED in profilesfx9: profile routes use CFG_FS_PROFILES_UNREADABLE_REASON, generic list split, zones/PcTools text completed]**

- `zones_http_post.c:72-75` (`zones_config_undecided` 409): "cfgfs format with force_healthy". It gives no `?`
  spelling, no erase warning and no reboot.
- `tools/PcTools/src/kilnctrl/zones_http_client.py:179-182` (`unreadable_store_refusal()`, the operator message for
  both 409s): "a cfgfs format_confirm with force_healthy", with no erase warning or reboot. It also offers scope
  kiln for every `store_unreadable_at_boot`, including the profile ones (MED-1).

Neither is in this commit's scope. They carry the same advice, which this commit corrected in only one place.

### INFO-1: "ALL cfg-stored settings" also takes data that is not a setting

A format also deletes `RELAY_CYCLES_FILE_PATH` (relay wear counters), the kiln config slots, aux outputs, iter-tune
and the CT-verify store (`kiln_scope_cfg_files.c:27-39`). "ALL" covers them, but an operator may not expect the wear
counters to be among "settings". Optional: "(including relay wear counters)".

### INFO-2: the not-truncated assertion compares against the macro itself

`test_profiles_http.c:2283` checks for `CFG_FS_FAVORITES_FILE_UNREADABLE_REASON "\"}"`. It proves the body is not
truncated (M6-M8), not the wording. The wording is covered by the strstr assertions next to it (M4, M5), so this is
fine.

### INFO-3: the generic-body truncation guard has 2030 B of headroom and is untested below 466 B

`:6191` can only fail under a stub buffer of 466 B or less. M7 (480 B) left it passing, and the favorites
assertions caught M7 instead. This is fine as long as both tests keep sharing the stub.

## Negtest

```
tools\negtest.ps1 -Command 'powershell -NoProfile -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir "{OUT}" -Only profiles_http' -Mutations <scratch>\rvfx8\mut.json -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
```

I used `-Command`, not `-Preset kilnfw-host`, because the preset builds every host test and negtest refuses
`-Preset` and `-Command` together. `-Only profiles_http` limits the build to the binary that holds every new
assertion. Base: `849c2bf36`. The baseline passed in 100.8 s. `real_tree_unchanged: true`,
`copies_removed: true`. No log contains `BUILD FAILURES` or a compiler error. Every row is a compiled mutant whose run
reported `RUN FAILURES` on the assertion shown.

| Mutation | Verdict | Decisive assertion(s) |
|---|---|---|
| M1 generic: drop ", then reboot" | CAUGHT | `:6188` query spelling + reboot |
| M2 generic: drop the "ERASES ALL ... preferences" clause | CAUGHT | `:6189` warns the format erases |
| M3 generic: `?force_healthy=1` -> ` force_healthy=1` | CAUGHT | `:6188`, `:6190` no space spelling |
| M4 favorites: drop "(then reboot)" | CAUGHT | `:2285` |
| M5 favorites: drop ", and also wipes Wi-Fi and kiln NVS" | CAUGHT | `:2286` |
| M6 stub buffer 2048 -> 512 (old size) | CAUGHT | `:2284` not truncated, `:2277` |
| M7 stub buffer 2048 -> 480 | CAUGHT | `:2284`, `:2277`, `:2286` |
| M8 stub buffer 2048 -> 547 (favorites body minus 1) | CAUGHT | `:2284` only: off-by-one truncation is detected |
