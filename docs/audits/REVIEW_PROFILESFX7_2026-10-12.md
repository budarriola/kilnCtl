# Review: profilesfx7 (eefff12b9) -- REVIEW_PROFILESFX6 MED-1 / LOW-1 / LOW-2

Adversarial review of origin/dev `eefff12b9`: the new `CFG_FS_FAVORITES_FILE_UNREADABLE_REASON` text
(`cfg_fs_refusal_http.h`), `s_fav_dirty = (err != ESP_OK)` in `profiles_favorites_set()`, comment fixes, and the new
test `test_favorites_failed_commit_keeps_dirty_so_reverse_still_writes`.

**Result: 0 HIGH, 0 MED, 3 LOW, 5 INFO.** The dirty-flag fix is correct and pinned. The remedies the text names
exist and work, but one is incomplete (LOW-1). Negtest: 5 mutants, 4 CAUGHT by assertion failures, 1 expected MISS.

## Questions from the brief

### (1) Where the reason text goes, and whether anything truncates it

- Firmware has one consumer: `cfg_fs_http_persist_failed_favorites()` passes the macro as part of a string literal to
  `httpd_resp_sendstr()`. No copy, no fixed buffer, no snprintf. `PROFILES_SAVE_ERR_MSG_LEN` (192) is used only by
  `ui_page_profile_builder_review.c` for profile-save errors and never holds this text. The LCD never renders it: the
  LCD's only `profiles_favorites_set()` caller (`ui_page_profile_picker.c` delete) logs the esp_err.
- The web page never shows it either. `favToggleBtn()` in `profiles_page.html` ignores the response
  (`.then(function () { return loadFavorites(); })`). See INFO-4.
- Lengths: the reason is 439 B and the full JSON body is 498 B. The only real limit is in the host-test harness:
  `test_profiles_http.c`'s `httpd_resp_sendstr` stub copies into `s_last_sendstr[512]` with snprintf, which truncates
  without any error. That leaves 13 B of room. See LOW-2.

### (2) Are the remedies correct?

- **Reboot re-reads: yes.** `main_boot_early.c` mounts cfg. Later, `main_network_http.c:417` calls
  `profiles_favorites_start()` -> `favorites_load()` -> `pref_cfg_fs_resolve()`. When `resolve_with_file()` reads the
  file cleanly it calls `unknown_set(path, false)`, which clears both the rev-unknown entry and the degraded entry.
- **factory_reset scope=profiles deletes prof_fav.bin: yes.** `profiles_scope_cfg_files.c` `kRootPaths[0]` is
  `PROFILES_FAVORITES_FILE_PATH`. The scope also erases `profiles_nvs`, which holds the legacy key, and then reboots
  (`reboot_task`). The text's "ERASES every user profile" is accurate.
- **format_confirm accepts `?force_healthy=1` as written: yes.** `format_confirm_post_handler()` calls
  `cfg_fs_confirm_uri_force_healthy(req->uri)`. It parses the query string, the first occurrence of the key wins, and
  only the exact value `1` counts. The handler ignores the body, so the old `force_healthy=1` spelling (with a space)
  could never have worked as a body field. The fix is right to use `?`.
- **A format alone does not end the refusal: LOW-1.**

### (3) Is the sibling `CFG_FS_FAVORITES_UNREADABLE_REASON` right to say "retry"?

Yes. `cfg_fs_http_persist_failed_favorites()` picks the sibling text only when the store is degraded,
`!pref_cfg_fs_rev_unknown(store)`, and `err == ESP_ERR_INVALID_STATE`. The file being unreadable always sets
rev-unknown, which routes to the FILE text. So the sibling is reached only on the `s_fav_user_unresolved` path: the
legacy NVS read failed and there is no file. On that path every `profiles_favorites_set()` calls `favorites_load()`
again under `s_save_lock` before refusing, so "this request re-reads it each time, so retry" is literally true. A cfg
format cannot help, because there is no file and the failing store is `profiles_nvs`. Only scope profiles or all
erases `profiles_nvs`, so that claim is also correct.

One reachable exception is harmless: during a factory reset `cfg_save_lock_reset_refused()` also returns
`ESP_ERR_INVALID_STATE`, and a reboot follows anyway.

A neighbouring case the FILE text does not cover: if the file is unreadable AND the legacy NVS read fails, a cfg
format removes the file. After a reboot the store then lands on the sibling path, where only scope profiles helps.
The FILE text names scope=profiles first, so an operator who follows it in order still recovers. INFO-5.

### (4) `s_fav_dirty = (err != ESP_OK)`: spurious writes, storms, lock interactions

- **No loop or storm.** `profiles_favorites_set()` has exactly three callers, and each runs once per user action:
  - the web favorite POST (`profiles_edit_http.c:1018`)
  - the web delete (`profiles_http.c:2242`, `(void)`, best-effort)
  - the LCD delete (`ui_page_profile_picker.c:209`, logged)

  Nothing retries automatically. The worst case is one extra cfg write per user action while the flag is dirty.
- **Content is right.** A dirty "no change" write commits the current RAM masks at `s_fav_rev + 1`. That is the same
  rev a failed post-rename write may already have left in the file. `pref_cfg_fs_save()` writes blind, with no rev
  comparison, so overwriting at an equal rev is allowed. Nothing treats "same rev, different content" as corruption.
- **Refusals also set dirty.** A rev-unknown or unmounted refusal never touched the file, yet it now sets dirty. From
  then on, every no-op request on that boot is attempted and refused with 409/503 where it used to answer 200. This
  is honest and is cleared on reboot. The two delete paths ignore the result. INFO-2.
- **Locks.** No new nesting: `s_save_lock` is still a leaf lock, and the delete path already called `set()` (and
  wrote, for a favorited id) before this change. `s_fav_dirty` is read and written under `s_save_lock`, except in
  `favorites_load()` when called from `profiles_favorites_start()`, which runs on the single-threaded boot path
  before httpd and the LCD start.
- **Test state leaks between tests.** `s_fav_dirty` is module-static and persists across host tests.
  `fuzz_small_setup()` calls `profiles_favorites_set(8, false)`, which now writes a favorites file whenever an
  earlier test left the flag dirty. No test depends on that today (M4 below breaks nothing). INFO-3.

## Findings

### LOW-1: "a cfg format ... clears it" does not end the 409 without a second reboot

`cfg_fs_confirm_format_job_run()` formats, re-registers and remounts, but nothing re-runs the stores' resolves. In
particular it does not clear `pref_cfg_fs.c`'s `s_unknown_paths[]` or the degraded table. Consider the usual
FILE-text case:

- the legacy NVS copy was adopted, so `s_fav_user_unresolved` is false;
- `profiles_favorites_set()` therefore never calls `favorites_load()` again;
- `prof_fav.bin` stays rev-unknown in RAM until the next boot.

So after `POST /api/cfgfs/format_confirm?force_healthy=1` succeeds ("ok -- cfg partition formatted and mounted"),
every favorite save still answers the same 409 until another reboot. The text tells the operator to reboot first and
format only "if it keeps failing", so the operator has already rebooted once. Nothing tells them a second reboot is
needed after the format. Factory reset reboots by itself, so the scope=profiles remedy is complete.

**Fix:** append "then reboot" to the format remedy, for example "... clears it (reboot afterwards) but ERASES ...".
Then add a `strstr` assertion for that phrase. Watch LOW-2's 13 B margin when doing so.

### LOW-2: the length assertion does not test anything; the real limit is a 512 B test buffer with 13 B to spare

`TEST_CHECK(strlen(s_last_sendstr) < PROFILES_SAVE_ERR_MSG_LEN * 4, ...)` compares against 768, but
`s_last_sendstr` is `char[512]` and filled by snprintf, so `strlen` can never exceed 511. The assertion cannot fail.

Firmware has no fixed limit (see (1)). The binding limit is the harness: the body is 498 B, so 13 B of growth would
truncate it. The tail assertions (`"scope kiln does not"` is the last phrase) would then fail spuriously, which is at
least loud. M2 demonstrated this: the old, longer wording made `test_profiles_http.c:2277` "says scope kiln does not
clear it" fail purely from truncation.

LOW-1's fix needs about 20 B, so it will hit this limit. **A margin test is needed in the harness, not in firmware:**

- either enlarge `s_last_sendstr` to 1024;
- or replace the dead bound with `TEST_CHECK(strlen(s_last_sendstr) < sizeof(s_last_sendstr) - 1, "body not truncated by the stub")` and assert that the body ends with `"}`.

Then the next wording change fails with an accurate message instead of a misleading one.

### LOW-3: the generic `CFG_FS_STORE_UNREADABLE_REASON` still has both MED-1 defects (out of this commit's scope)

The sibling generic text, used for every other store's `store_unreadable_at_boot` 409, still:

- sends the operator to "a cfg format (POST /api/cfgfs/format_confirm force_healthy=1)" with no warning that the
  format erases every user profile, the zones config and every cfg preference;
- uses the space spelling, which as written is a refused POST. The handler reads only `?force_healthy=1` from the
  URI, so a body or loosely-read `force_healthy=1` gets the 409 "cfg is mounted and healthy". The 409 does at least
  name the right spelling.

REVIEW_PROFILESFX6 MED-1 named exactly these two defects for the favorites text, and they apply equally here. The
same LOW-1 caveat also applies: after a format, a rev-unknown store stays refused until a reboot.

### INFO-1: no test pins "a clean no-op does not write"

M4 (`s_fav_dirty = true` always) is MISSED. A regression that leaves the flag stuck would add one cfg write per
favorites no-op, including every web or LCD delete of a non-favorited profile, and no test would notice. That is cheap
(user-triggered only, see (4)), so this is not a defect.

Pinning it would take one assertion: count writes through `pref_cfg_fs_set_write_fn()` across a no-op
`profiles_favorites_set()` on a clean store.

### INFO-2: after one refusal, no-op favorite requests answer 409 for the rest of the boot

This is intended (see (4)). It is noted because before this change an unfavorite of a non-favorite answered 200 even
on a refused store.

### INFO-3: `s_fav_dirty` is not reset between host tests

Each test's setup inherits the previous test's flag. This is harmless today. A `profiles_favorites_start()` in
`fuzz_small_setup()` would make each test independent of the order tests run in.

### INFO-4: the web page drops the favorite POST's response

`favToggleBtn()` (`profiles_page.html`) reloads the favorites and re-renders, but never reads the status or body. A
409 or 503 looks like "the star did not change", and the long reason text reaches only API and MCP clients. This
predates the change.

### INFO-5: "(scope=all does that too)" understates scope=all

Besides formatting cfg, scope=all also erases Wi-Fi and kiln NVS. The mixed case from (3), where the file is
unreadable and the NVS read also fails, is cleared only by scope profiles or all. A format alone moves that store to
the sibling text. The text's ordering, with scope=profiles named first, covers this.

## Negtest

```
tools\negtest.ps1 -Command 'powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir "{OUT}" -Only profiles_http' -Mutations <scratch>\rvfx7\mut.json -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
```

`-Command` was used rather than `-Preset kilnfw-host`, because negtest refuses `-Preset` and `-Command` together and
the run had to be limited to `-Only profiles_http`. Base: `ecc2fa7c1` (origin/dev, which contains `eefff12b9`).

The unmutated baseline passed in 51.3 s. `real_tree_unchanged: true`, `copies_removed: true`. No mutant hit BUILD
FAILURES: every CAUGHT row is a compiled mutant whose run reported `RUN FAILURES` with the assertion lines below.

| Mutation | Verdict | Decisive assertion(s) |
|---|---|---|
| M1 `s_fav_dirty = false;` (the LOW-1 bug) | CAUGHT | `test_profiles_http.c:2308` "after reboot 3 is not a favorite (the reverse change was written)" |
| M2 old "this request re-reads it each time, so retry (a reboot also retries)" wording | CAUGHT | `:2278` "first remedy is a reboot", `:2280` "does not promise a per-request re-read", and `:2277` "says scope kiln does not clear it" (the last only from stub truncation, see LOW-2) |
| M3 (own) `s_fav_dirty = (err == ESP_ERR_INVALID_STATE);` (dirty only on refusal, not on a write failure) | CAUGHT | `:2308` |
| M4 (own probe) `s_fav_dirty = true;` always | MISSED (expected) | none; see INFO-1 |
| M5 (own) `format_confirm?force_healthy=1` -> `format_confirm force_healthy=1` | CAUGHT | `:2281` "force_healthy is the query string" |
