# Review: PROFILESFX6 (bebc85025), 2026-10-12

Reviewer: Opus. Commit `bebc85025` ("favorites: publish RAM only after a successful commit; correct 409 advice for an
unreadable prof_fav.bin"), which fixes `REVIEW_PROFILESFX4_2026-10-12.md` LOW-A and LOW-B:

- `cfg_fs_refusal_http.h`: new `CFG_FS_FAVORITES_FILE_UNREADABLE_REASON`, sent by
  `cfg_fs_http_persist_failed_favorites()` when `prof_fav.bin` is rev-unknown.
- `profiles_favorites.c`: `profiles_favorites_set()` publishes RAM only after `pref_cfg_fs_commit()` returns
  `ESP_OK`, and `s_fav_dirty` is reset to false on every commit path.
- `test_profiles_http.c`: new `test_favorites_refused_commit_leaves_ram_unchanged`. The unmounted test now asserts
  that a refused save is not published.

Review tree: `C:\wt\rvprofilesfx6_pk1j5q` at origin/dev `21cc28ad7` (contains `bebc85025`; no later commit touches
the three files).

## Verdict

0 HIGH, 1 MED, 2 LOW, 4 INFO.

- LOW-B is fixed as described. The publish-after-commit ordering is correct, and readers stay consistent.
- LOW-A is half fixed. Scope kiln is gone from the text, and scope profiles/all are correct. The new text has two
  problems (MED-1):
  - it says "this request re-reads it each time, so retry", which is false in the exact case LOW-B covers;
  - it offers a cfg format as the first remedy without saying that a format erases the whole cfg partition.
- Dropping the dirty flag loses one real case (LOW-1). A host test demonstrated it: an unfavorite answers OK, and the
  favorite comes back after a reboot.

## Answers to the brief's questions

- **Does factory_reset scope=profiles/all delete prof_fav.bin?** Yes.
  - Scope profiles runs `profiles_scope_cfg_files_delete()`. Its `kRootPaths[0]` is `PROFILES_FAVORITES_FILE_PATH`
    (`profiles_scope_cfg_files.c:35-39`, delete loop `:139-150`).
  - Scope all formats the whole cfg partition (`factory_reset.c:15`, comment at `:103-110`).
  - Scope kiln only sets `delete_kiln_cfg_files` and does not touch the file.
- **Does `format_confirm` with `force_healthy=1` exist?** Yes, as a query-string parameter only:
  `cfg_fs_confirm_uri_force_healthy()` parses `req->uri` after `?` (`cfg_fs_format_gate.c:276-295`), and a body
  field is ignored. The text writes "POST /api/cfgfs/format_confirm force_healthy=1", so a client that sends it in the
  body gets the "mounted and healthy" refusal. That refusal names the right form
  (`cfg_fs_format_http.c:97`), so the failure is recoverable (INFO-1). The generic `CFG_FS_STORE_UNREADABLE_REASON`
  uses the same notation.
- **Did any caller rely on RAM updating despite a failed commit?**
  - HTTP favorite POST (`profiles_edit_http.c`): it refuses an unmounted cfg with 503 before calling `set()`, and any
    other failure was already an error response. The response is unchanged; only the RAM side effect is gone, which
    is the point of LOW-B.
  - Delete paths: `profiles_http.c:2242` (best effort, result ignored) and `ui_page_profile_picker.c:209` (logged).
    When the commit fails, the deleted slot's favorite bit now stays in RAM. `favorites_list_get_handler()` filters on
    `profiles_slot_used()` (`profiles_catalog_http.c:622-630`), and the picker lists only used slots, so nothing
    visible changes. The bit matches the file, and the file is what a reboot would load anyway (INFO-2).
  - Backup import, legacy NVS migration and the boot load do not call `set()`. Boot load uses `favorites_load()`,
    which is unchanged.
  - There is no LCD favorite toggle. The picker only reads favorites and clears one on delete.
- **Unmounted cfg: are favorites now impossible to set? Is that a regression vs NVS-authoritative behaviour?** Not a
  regression.
  - The NVS writer was retired when the dual-write window closed (`profiles_favorites.c:354-356`), before this
    commit. On an unmounted board a favorite has not persisted since then.
  - The HTTP route already refused with 503 before reaching `set()`. Only a direct (delete-path) caller saw the
    old "live until reboot" behaviour.
  - What changed is that a board whose cfg is never mounted can no longer show a favorite for the current boot.
    That is consistent with the owner decision that cfg is the only target (INFO-3).
- **Locking/race.**
  - `fav_publish()` now runs after `pref_cfg_fs_commit()`, still under `s_save_lock`. Every writer of `s_fav_*`
    (`set()`, and `favorites_load()` reached from `set()`) holds that lock.
  - Readers (`profiles_favorites_is()` / `_masks()`) copy under `s_fav_mux`, so no torn mask is possible.
  - Between a successful commit and the publish, a reader sees the pre-change mask for a few instructions. That is
    harmless, and it is the order every other store uses.
  - `favorites_load()` from `profiles_favorites_start()` runs without `s_save_lock`, but only at boot before httpd
    registers its routes. That is unchanged.
  - No new race.

## Findings

### MED-1 (FIXED, profilesfx7): the new 409 text tells the operator to "retry" in a case where retry cannot work, then escalates to a cfg format without saying the format erases every profile and every other cfg-backed setting

- **Where:** `firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h:53-57` (`CFG_FS_FAVORITES_FILE_UNREADABLE_REASON`),
  sent from `:106-108`.
- **Retry claim:** "this request re-reads it each time, so retry". `profiles_favorites_set()` re-reads only while
  `s_fav_user_unresolved` is set (`profiles_favorites.c:322-328`).
  - With a valid legacy NVS copy and an unreadable file, `pref_cfg_fs_resolve()` adopts the NVS copy and returns
    `have=true` (`pref_cfg_fs.c:473-480`). So `favorites_load()` takes the resolved branch, `s_fav_user_unresolved`
    is false, and no request ever re-reads the file.
  - `pref_cfg_fs_save()` then refuses on rev-unknown (`pref_cfg_fs.c:373-378`, whose own log says "reboot to
    retry").
  - This is exactly the LOW-B scenario. The new test `test_favorites_refused_commit_leaves_ram_unchanged` shows two
    sets in a row both refused. Only the no-NVS-copy (unresolved) case re-reads per request.
  - The suggested text in the PROFILESFX4 review already carried this inaccuracy, and the fix copied it.
- **Format advice:** "clear it with a cfg format (...) or factory_reset scope=profiles (or scope=all, which ALSO
  erases every user profile)". A cfg format (`cfg_fs_confirm_format_device()`, `cfg_fs_mount.c:563-590`) formats
  the whole `cfg` LittleFS partition.
  - Since the dual-write close, that partition is the only copy of the user profiles, zones config and the other
    migrated stores. A format destroys more than scope=profiles does, and about as much as scope=all, whose cfg step
    is that same format.
  - The sentence attaches the "ALSO erases every user profile" warning only to scope=all. That presents the format as
    the cheap option.
- **Failure scenario:**
  1. A board with legacy `prof_favusr` keys gets a transient read error on `prof_fav.bin` at boot.
  2. The operator taps favorite, gets the 409, retries several times as told, and gets the same 409 each time.
  3. Following "if it keeps failing, clear it with a cfg format", they run `format_confirm?force_healthy=1`.
  4. Every saved profile and the zones/PID config on cfg are erased, to clear a favorites file that a plain reboot
     would have re-read.
- **Fix:**
  - Say "reboot to retry". Mention per-request re-reads only if the handler can tell the two cases apart, which it
    can: `rev_unknown && !unresolved`.
  - Order the remedies from least destructive: reboot, then factory_reset scope=profiles (erases user profiles and
    favorites), then cfg format, stating that a format erases ALL cfg-stored settings (profiles, zones, preferences).
  - Write the parameter as `?force_healthy=1`.
  - Add a test for the adopted case that pins "reboot" and forbids "each time".

### LOW-1 (FIXED, profilesfx7): dropping `s_fav_dirty` lets a reversed request report OK while the file keeps the change (demonstrated)

- **Where:** `firmware/KilnFW/App/drivers/persist/profiles_favorites.c:364` (`s_fav_dirty = false;` on every path)
  with `:347` (`if (!changed && !s_fav_dirty) return ESP_OK;`).
- **Cause:** `cfg_fs_write_atomic()` can fail after the rename, when the read-back verify fails
  (`cfg_fs.c:752-765`). At that point the new bytes are probably at `final_path` ("the old file ... is already
  gone"), but the caller gets `ESP_FAIL`. After `bebc85025`, RAM keeps the old mask and the dirty flag is cleared.
  The flag existed for "the file may differ from RAM".
- **Failure scenario:**
  1. Favorite 3: the write lands and the verify fails. HTTP answers 500, and RAM still says "not favorite".
  2. The user, or a delete-path clear, unfavorites 3. `changed=false`, `dirty=false`, so `set()` returns `ESP_OK`
     without writing.
  3. On reboot, `prof_fav.bin` holds rev N+1 with 3 favorited, and the favorite reappears even though the last
     request reported success.
  4. Before `bebc85025`, RAM held the change and dirty forced the unfavorite to write.
- **Evidence:** a temporary host test in the review tree used `pref_cfg_fs_set_write_fn()` with a writer that calls
  `cfg_fs_write_atomic()` and then returns `ESP_FAIL`. It ran set(3,true) (FAIL), then set(3,false), which returned
  OK, then `profiles_favorites_start()`. Result: `FAIL test_profiles_http.c:2304: after reboot 3 must not be a
  favorite`, 1698/1699. The test was removed afterwards. It is not committed.
- **Rarity:** this needs a post-rename verify failure, so it is rare.
- **Fix:** keep publish-after-commit (LOW-B), but restore `s_fav_dirty = (err != ESP_OK);`. The meaning becomes
  "file state unknown, the next request must write even when RAM says no change".
  - For a pre-write refusal (unmounted, rev-unknown, reset fence), a no-op request then reports the refusal instead
    of OK, which is more honest.
  - Mutation N4 (below) shows that restoring this line breaks no existing test.

### LOW-2 (FIXED, profilesfx7): stale contract text after the behaviour change

- `profiles_favorites.h:51-52`: "A save while cfg is unmounted fails ...; the change still applies live and reverts
  on reboot." This is no longer true.
- `profiles_favorites.h:75-77`: "a non-OK return ... means the change IS live but did not reach flash." Now the
  opposite holds: a non-OK return means RAM is unchanged.
- `profiles_favorites.c:203-206`: the comment says `s_fav_dirty` "stays false ... Kept so a retry ... must still
  write". The variable is now dead (always false), so the `!s_fav_dirty` term at `:347` is vacuous. Remove it, or
  revive it per LOW-1.
- `pref_cfg_fs.c:400-401`: `pref_cfg_fs_commit()`'s log says "the value lives in RAM until reboot". That is now false
  for favorites. It is shared with other stores, so qualify it rather than delete it.

### INFO-1: `force_healthy=1` is a query parameter

The text (and the generic `CFG_FS_STORE_UNREADABLE_REASON`) writes "POST /api/cfgfs/format_confirm force_healthy=1".
Only `?force_healthy=1` in the URI counts (`cfg_fs_format_gate.c:276-295`). The refusal for a body-only form names
the right spelling, so this is cosmetic. It is folded into the MED-1 fix.

### INFO-2: a failed favorite clear on delete now leaves the bit in RAM

`profiles_http.c:2242` / `ui_page_profile_picker.c:209`: when the clear's commit fails, the deleted slot keeps its
bit in RAM, equal to the file. All readers filter on the used bitmap, and the file already had the bit before this
commit (so it returned after a reboot), so nothing user-visible changes. This is the INFO-E class from PROFILESFX4.

### INFO-3: an unmounted board can no longer show a favorite even for the current boot

This is intended (LOW-B), and HTTP already refused with 503 first. It is recorded because the header still documents
the old behaviour (LOW-2).

### INFO-4: the full kilnfw-host preset was not used for the negtest

Per the brief, the dev baseline may be red in `test_autotune_engine_prestart.c`. The negtest used `-Command` with
`build_host_tests.ps1 -OutDir {OUT} -Only profiles_http`, and the baseline passed. Every mutation touches only code
that `test_profiles_http.c` exercises.

## Negtest

`tools\negtest.ps1 -Command 'powershell ... build_host_tests.ps1 -OutDir "{OUT}" -Only profiles_http' -Mutations <appendix>
-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.

Baseline (unmutated) PASS in 25.5 s. The run reported no "REAL TREE CHANGED". Every CAUGHT row below is a mutant that compiled and failed a test assertion; none was caught by a build failure.

| Mutation | Verdict | Decisive assertion(s) |
|---|---|---|
| N1 publish before commit | CAUGHT | `test_profiles_http.c:2306` "refused commit did not publish favorite 3", `:2309`, `:2588`, `:2591` |
| N2 publish even on failed commit | CAUGHT | same four as N1 |
| N3 no publish on success | CAUGHT | `:6227`, `:2511`, `:2576`, `:2352` and five others |
| N4 `s_fav_dirty = (err != ESP_OK)` | MISSED (expected) | no test fails. Restoring the dirty flag (the LOW-1 fix) breaks nothing that exists today, and no test pins the dirty semantics either way |
| N5 rev-unknown gets the key-store text | CAUGHT | `:2277` "says scope kiln does not clear it" |
| N6 rev-unknown falls through to the generic text | CAUGHT | `:2276`, `:2277`, `:2278` |

The LOW-B and LOW-A fixes are pinned by real assertions. No test pins the "retry"/"reboot" wording (MED-1).

## Appendix: mutations (JSON)

```json
[
 {"name":"N1_publish_before_commit",
  "file":"firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find":"        cur_builtin = updated;\n    }\n",
  "replace":"        cur_builtin = updated;\n    }\n    fav_publish(&cur_user, cur_builtin);\n"},
 {"name":"N2_publish_even_on_failed_commit",
  "file":"firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find":"    s_fav_dirty = false; /* a refused/failed commit",
  "replace":"    fav_publish(&cur_user, cur_builtin); s_fav_dirty = false; /* a refused/failed commit"},
 {"name":"N3_no_publish_on_success",
  "file":"firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find":"        fav_publish(&cur_user, cur_builtin); /* PROFILESFX4 LOW-B",
  "replace":"        (void)0; /* PROFILESFX4 LOW-B"},
 {"name":"N4_dirty_flag_restored",
  "file":"firmware/KilnFW/App/drivers/persist/profiles_favorites.c",
  "find":"    s_fav_dirty = false; /* a refused/failed commit",
  "replace":"    s_fav_dirty = (err != ESP_OK); /* a refused/failed commit"},
 {"name":"N5_revunknown_gets_key_text",
  "file":"firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h",
  "find":"\"reason\\\":\\\"\" CFG_FS_FAVORITES_FILE_UNREADABLE_REASON",
  "replace":"\"reason\\\":\\\"\" CFG_FS_FAVORITES_UNREADABLE_REASON"},
 {"name":"N6_revunknown_falls_to_generic",
  "file":"firmware/KilnFW/App/drivers/common/cfg_fs_refusal_http.h",
  "find":"cfg_fs_degraded_is(store) && err == ESP_ERR_INVALID_STATE) {\n        httpd_resp_set_type",
  "replace":"cfg_fs_degraded_is(store) && err == ESP_ERR_INVALID_STATE &&\n        !pref_cfg_fs_rev_unknown(store)) {\n        httpd_resp_set_type"}
]
```
