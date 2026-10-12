# REVIEW_PROFILESFX3 (2026-10-12)

Review of the REVIEW_PROFILESFX LOW-1..3 fixes and the WEBGENFX test gaps H3b, H5b and H7 on origin/dev:

- `3e1355a8a` Fix REVIEW_PROFILESFX LOW-1..3 and WEBGENFX test gaps H3b/H5b/H7 (the main fix)
- `f8c171ce6` Pin LOW-1 test: set dirty so a nulled lastGen shows the false banner
- `3bee3a583` Strengthen H3b zones generation seed-0 test

Reviewed at `3bee3a583` in worktree `C:\wt\rvpfx3_8d5p4j`. No code was changed.

## Summary

| ID | Severity | Area | Finding |
|----|----------|------|---------|
| MED-1 | MED | PcTools LOW-2 | `_error_saved()` never sees the body on a real HTTP connection. The fix does nothing on hardware. The tests pass only because the fake body is a seekable `BytesIO`. |
| LOW-1 | LOW | LCD LOW-3 | The new refusal text from `profiles_http.c` is 93 chars. Its only caller passes `err_msg[64]`, so the LCD shows a cut-off sentence. The "192-byte buffer" test checks the wrong buffer. |
| INFO-1 | INFO | web LOW-1 | No test covers the case where the banner should show (generation moved and dirty after a failed decide). The logic is right. |
| INFO-2 | INFO | web | The page still ignores `saved:true` on a failed decide. A user can press Save as again and make a duplicate. This was there before. |

## Fix status

| ID | Status | Fix |
|----|--------|-----|
| MED-1 | fixed in 603f5283c | `profile_live_http_client._error_info` reads the body once; `saved` must be literally true with an int id. Non-seekable fakes plus loopback `http.server` tests. Negtested (seek parse, dropped check, truthy check: all CAUGHT). |
| LOW-1 | fixed in 603f5283c | `PROFILES_SAVE_ERR_MSG_LEN` (128) in `profiles_store.h`; the LCD save dialog uses it; host test checks the real caller. Negtested (CAUGHT by assertions). Stack: +64 B in the LVGL task, not measured on hardware. |
| INFO-1 | fixed in 603f5283c | New scenario in `test_live_profile_action_errors.js`. Negtested (CAUGHT). |
| INFO-2 | open | Web page still ignores `saved:true` on a failed decide. |

None of the changes is HIGH. LOW-1 (web), H3b, H5b and H7 are correct and pin real behavior. Negtest results are below.

## 1. LOW-1 (web): keeping lastGen after a failed decide

The change is correct, and it adds no lost-update path.

- Every write the page sends carries `?gen=lastGen` (Save and decide). The board refuses a stale generation with 409. Keeping a stale `lastGen` can therefore only cause a refusal. It cannot cause an overwrite.
- After a failed decide, `refreshLive()` compares `live.generation !== lastGen`:
  - If the generation moved (for example a `LIVE_DECIDE_SAVED_BUT_EDITED` 409, where another client edited the copy during the save) and the form is dirty, `showStaleBanner()` runs. That is the right result, because the copy really did change elsewhere.
  - If the generation moved and the form is clean, the page reloads the copy.
  - If the generation did not move, nothing happens, and the unsaved edits stay without a banner. This is the false banner LOW-1 removes.
- The old code set `lastGen = null`. Because `null !== live.generation` always holds, a dirty form always showed "Changed elsewhere", even when nothing had changed.

**INFO-1.** `test_live_profile_action_errors.js` pins only the "generation unchanged" case. The live mock returns `generation: 1` with `lastGen = 1`. No scenario covers a failed decide followed by a moved generation with a dirty form, which should still show the banner. A mutation that suppressed the banner after a failed decide would not be caught here. Add one scenario where `scenario.live.generation` is 2.

**INFO-2.** `decide()` shows `reason || error` and does not read `saved`/`id`. The 409/500 "saved to slot N" text is visible to the user, but the page does not change state on it (for example by disabling Save as). This is not a regression. It is the web counterpart of MED-1.

## 2. MED-1: LOW-2 parse never reaches the body on real HTTP

`profile_live_http_client._post_form()` calls `_error_detail(exc)` first. That function does `exc.read()` and consumes the body. `_error_saved(exc)` then calls `exc.fp.seek(0)`. On a real `urllib` `HTTPError`, `fp` is an `http.client.HTTPResponse`, which cannot seek. `seek(0)` raises `io.UnsupportedOperation`, the bare `except Exception` swallows it, and the function returns `(False, None)`.

I checked this against a real socket. A local `http.server` returned 409 with `{"ok":false,"saved":true,"id":7,"error":"saved to slot 7; edited during save"}`. `decide_live_save_as()` raised `ProfileLiveHttpError` with `status 409`, the right `detail`, and **`saved False`, `saved_id None`**. So `profile_live_decide` still prints `error: ...` and never prints the new "SAVED to slot N ... do not retry save_as" text.

The tests are false-green. `_fake_http_error()` builds the `HTTPError` with `fp=io.BytesIO(body)`, which can seek, so `seek(0)` rewinds and the second read works. `test_saved_and_id_reach_the_exception` passes on a path that real traffic never takes.

The failure is safe in one direction. The parse cannot falsely report SAVED:
- It needs `obj.get("saved") is True` exactly.
- `id` must be a non-bool `int`, or the result is "a slot (id not reported)".
- Malformed JSON, or a body without `saved`, gives `(False, None)`.

The risk is the reverse. The SAVED case comes back as a generic error, and a caller that retries save_as makes a duplicate profile. That is exactly what LOW-2 was meant to stop. The "saved to slot N" prose is still in `detail`, which softens this.

Fix: read the body once. `_error_detail` can return the raw text, or the body can be cached on the exception. Then parse `saved`/`id` from that same text. Make the test fake match the real case, either with a non-seekable stream (a `BufferedReader` over a pipe, or a `BytesIO` subclass whose `seekable()` is False and whose `seek` raises) or with a real loopback `http.server`. Then negtest it.

## 3. LOW-1: LCD refusal text truncated to 63 chars; the test checks the wrong buffer

Text in `3e1355a8a`:

- `profiles_http.c` (in-lock rev check): "This profile was changed since you opened it. Reopen it to edit, or save to a different slot." (93 chars)
- `ui_page_profile_builder_review.c` (pre-check, a literal in the dialog): "That slot changed since you opened this draft. Reopen the profile, or save to a different slot." (95 chars)

The only caller of `profiles_http_save_expect_rev()` (so the only path to the `check_rev` refusal) is `ui_page_profile_builder_review.c:124`. It passes `char err_msg[64]`. `snprintf` cuts the text safely to "This profile was changed since you opened it. Reopen it to edit". The "or save to a different slot" half of the advice, which was the point of LOW-3, is lost on the one screen that shows it.

The new test assertion `strlen(err) < 192 - 9` ("fits the LCD 192-byte message buffer after 'Refused: '") is about `ui_page_live_decide.c`'s `msg[192]`, which never gets this text. The test passes its own buffer (`sizeof(err)`), so it cannot see the 64-byte cap.

Fix: either raise `err_msg` in `do_save_apply` to at least 128 bytes, or shorten the text to under 64. Then make the test assert against the real caller's size, ideally through a shared `#define` both files use.

**LCD fit at 480x320, no scrolling.** The dialog is an `lv_msgbox` 400 px wide (`UI_CONFIRM_WIDTH_PX`) in Montserrat 14 (20 px line, `UI_THEME_FONT_LINE_HEIGHT_PX`). At about 7 px per character, 95 characters wrap to two lines, about 40 px. With the title and the 48 px footer, the dialog fits well inside 320 px. Other dialogs already show longer text, such as the 192-byte "Refused: ..." on the live-decide page. This is an estimate from the source, not a camera capture. I see no scroll risk. The fix above (a bigger buffer) adds at most one more line.

Locale/format paths: the firmware has no localisation. Both strings are fixed ASCII with no `%s` arguments, so their length does not change at run time.

## 4. H3b / H5b / H7 and the fake_kv helper

- **H7 (`test_boot_seed_is_even_parity`).** Pins real behavior. It scripts an odd RNG word (`0xFFFFFFFF`), runs `profiles_http_start()`, and asserts every slot rev is even. `profiles_http_start()` seeds and runs `profiles_boot_load()` (begin/end seqlock, +2 per slot) before it returns `ESP_ERR_INVALID_STATE` for the missing HTTP server, so the test reaches the code it checks. Dropping the `e` in `0x3ffffffeu` makes the start seed odd, and the end rev is then odd. CAUGHT (below).
- **H3b (`test_zones_http.c`, after `3bee3a583`).** Pins real behavior. Seeds 0 and 1 both map to generation 1 through `| 1u`. Without the OR, they start one apart, and identical start-up bumps keep them apart. The first version only checked `!= 0`. That was weak, because start-up bumps can make the count nonzero even with a seed of 0, and it was MISSED. The strengthened version is CAUGHT (below).
- **H5b (`test_legacy_erase_verify_probe_error_is_failure`).** Pins real behavior. `legacy_nvs_erase_keys()` calls `hal_kv_key_exists()` exactly twice for one key: the first probe at line 32, then the verify probe at line 54. `skip=1, count=1` passes the first probe through and fails only the verify probe with `HAL_IO`. Weakening the verify check to `ex == HAL_OK` is CAUGHT (below).
- **`fake_kv_script_key_exists_status_after(skip, count, status)`.** This is a fault-injection counter, not a model of KV semantics, so it cannot diverge from the real KV. Points checked:
  - The skip is used up only after the NULL-handle and NULL-key checks, the same as `count`, so invalid calls do not shift the schedule.
  - Skipped probes do the real lookup.
  - `fake_kv_reset_all()` and the old `fake_kv_script_key_exists_status()` both clear `s_key_exists_fail_skip`, so old callers act as before.
  - The test clears the script afterwards.
  - Note: with `count == 0` a non-zero skip still counts down harmlessly. No caller uses that.

## 5. Negative tests (`tools\negtest.ps1`, throwaway copies at base `3bee3a583`)

| Mutation | File | Command | Verdict |
|----------|------|---------|---------|
| LOW1: re-add `lastGen = null;` in the failed-decide branch | `live_profile_page.html` | `node firmware\KilnFW\App\test\test_live_profile_action_errors.js` | CAUGHT (`FAIL: failed decide keeps lastGen (got null)`) |
| H7: profile seed mask `0x3ffffffeu` -> `0x3fffffffu` | `profiles_http.c` | host, `-Only "test_profiles_http\.c"` | CAUGHT (`test_profiles_http.c:2658` "every slot rev is even after boot ..." plus 11 seqlock asserts) |
| LOW3: restore "That slot changed since you opened it. Pick the slot again." | `profiles_http.c` | host, `-Only "test_profiles_http\.c"` | CAUGHT (`test_profiles_http.c:2632`) |
| H3b: drop `\| 1u` from the zones seed | `zones_http.c` | host, `-Only "test_aux_outputs_store\|test_zones_http\.c"` | CAUGHT (`test_zones_http.c:3427`) |
| H5b: verify loop `ex != HAL_NOT_FOUND` -> `ex == HAL_OK` | `legacy_nvs_erase.h` | host, same | CAUGHT (`test_aux_outputs_store.c:153`) |
- **JS preset baseline.** `tools\check_page_js_tests.ps1` fails on this base: 58 of 59 pass, and `test_zones_type_toggle.js` fails with `kgR6` not defined, which is being fixed elsewhere. As instructed, the LOW-1 mutation therefore ran `node` on `test_live_profile_action_errors.js` directly through `negtest.ps1 -Command`. The baseline passed and the real tree was unchanged.
- **Host mutations.** These ran through `negtest.ps1 -Command` with `build_host_tests.ps1 -OutDir "{OUT}" -Only "test_aux_outputs_store|test_zones_http\.c"`, which selects the main host executable (it holds `test_profiles_http.c` and `test_aux_outputs_store.c`) and the separate zones executable. Each mutation used the expect pattern `(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`. Both host runs had a passing baseline and finished with "real tree unchanged" and the copies removed (logs `%TEMP%\negtest_logs\20261012_031023_xg8d` and `20261012_032222_mnod`). The first host run missed the profiles executable: `test_profiles_http.c` is its own seventh executable, and the `-Only` regex did not select it. Its H7 and LOW3 MISSED verdicts were a harness error and were rerun with `-Only "test_profiles_http\.c"`, where both were CAUGHT. The four results in the table are the clean replacements for the fixer's REAL-TREE-CHANGED runs.
- An earlier host attempt ended BASELINE-FAILED at once (exit 97). The nested `powershell -File` call stripped the inner quotes, so the `|` in `-Only` became a pipe. That was a harness error, not a test result, and it was rerun with a direct `& .\tools\negtest.ps1`.
- LOW-2 (MED-1) was not negtested. A loopback reproduction showed that the shipped code already fails on real HTTP (section 2).
