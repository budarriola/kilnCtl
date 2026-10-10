# Web UI JS coverage gaps, 2026-10-10

Scope: `firmware/KilnFW/App/drivers/http/*.html|js` and `drivers/net/*_page.html`, against
`firmware/KilnFW/App/test/test_*.js` (run by `tools/check_page_js_tests.ps1`) and
`lint_pages.js`. `zones_page.html` and app.js's password-reset flow were out of scope
(another fixer owns them).

Existing style: most tests are regex/source-contract tests or extract one function. Few
EXECUTE a click handler, so "what text does the operator see for HTTP code X" was mostly
unpinned. This sweep adds `_page_vm.js` (vm + fake DOM; runs a page's last inline script)
and four executing tests (items 1-4 below).

## Ranked list (impact on a kiln operator, then likelihood)

| # | Behaviour | Status |
|---|-----------|--------|
| 1 | Settings destructive actions (factory-reset scopes, software reset, cfg-fs format): confirm gate, request body, `Failed: <server text>` for every refusal, no success line on failure, catch-arm wording | TESTED `test_settings_reset_flows.js`; defects D1, D2 |
| 2 | Backup restore: dry-run refusal text, cancel, `X-Kiln-Config-Ack-Delete` count, success vs refusal for 400/403/409/413/428/500, auth cancel, network failure | TESTED `test_backup_restore_flow.js`; defect D3 |
| 3 | C/F conversion: `kcUnit.toDisplay/fmt` offset, null/NaN, bad `temp_unit`, `set()` body; `main_page` `fmtDeltaC` must not add 32 | TESTED `test_kcunit_conversion.js` |
| 4 | Live (mid-firing) profile edit: save 400 bound / 409 window text verbatim, 200 with `ok:false`, overwrite checkbox gate, save-as name encoding, fork refusal | TESTED `test_live_profile_action_errors.js`; defects D4, D5, D6 |
| 5 | Unsaved-edit loss: no page registers `beforeunload` (0 hits in every html/js), so navigating away from the profile editor, wizard, safety config or kiln-configs forms silently drops edits; also pollers that re-render over an open form | UNTESTED; defect D7 (page-wide) |
| 6 | Session expiry on polling pages: a 401 from a background `setInterval` poll surfaces as a generic "Could not reach the board" (see D4's catch arms); `test_login_auth_wrapper.js` covers the fetch wrapper but no page's poll catch arm | UNTESTED |
| 7 | Mixed units inside one view: `profiles_page.html:1960` renders targets in the display unit while the editing inputs stay raw degrees C (app.js kcUnit comment: "inputs send raw C"), so an operator in F reads 1832 F and types into a box that expects 1000 | UNTESTED; design hazard, not a code bug |
| 8 | `main_page.html` run controls (start, stop, clear trip, pause) per refusal code, including the 409 system-mode gate text and the 428 interlock | UNTESTED (258 KB page, 84-handler class with no executing test) |
| 9 | `security_page`, `wifi_provision_page`, `ota_page` status-code handlers (31 `.ok/.status` sites): password change, Wi-Fi join, stage/install refusal text; only the OTA poll-auth and update-page cards have tests | UNTESTED |
| 10 | Mojibake / encoding: no check catches double-encoded UTF-8 in page sources; D6 shipped unnoticed | UNTESTED; suggest a `lint_pages.js` rule |

## Defects found (reported, not fixed; pages are not touched by this change)

- D1. `settings_page.html` reset, software-reset and cfg-fs-format handlers: when the sign-in
  prompt is cancelled the catch arm `return`s silently, leaving the status line on the
  in-progress text ("Erasing and rebooting...", "Rebooting both processors...",
  "Formatting...") although nothing was sent. `backup_page.html` already handles this with
  "Sign-in cancelled -- nothing was restored."; copy that.
- D2. Same handlers (factory reset and software reset): every non-auth rejection, including a
  transport failure before the request left the browser, shows "Request sent -- the board may
  already be rebooting, so this connection dropped." That is true only after a send; a
  failed send reads as a reboot in progress. (The format handler's wording is correct.)
- D3. `backup_page.html`: a 500 from `/api/backup/import` is a partial write, but the page
  prints "Restore refused: <text>". "Refused" implies nothing changed. It should say the
  restore may be partially applied. Test pins only "no success claim, server text shown".
- D4. `live_profile_page.html` save/fork/decide: `r.json()` runs before any status check, so a
  refusal with a non-JSON body (401/403 CSRF or auth refusal, a 500 page) lands in the catch arm
  and shows "Save failed -- could not reach the board." Wrong reason for a refusal.
- D5. `live_profile_page.html`: sign-in cancel on save leaves "Saving..." (same class as D1);
  "Discard working copy" posts immediately with no kcConfirm, one click loses all edits.
- D6. `live_profile_page.html` has four double-encoded UTF-8 ellipses ("Loading..." etc.
  bytes `c3 a2 e2 82 ac c2 a6`); the browser renders "Loadingâ€¦", "Savingâ€¦", "Forkingâ€¦",
  "Workingâ€¦". No other page matched the byte pattern. (It is also visible in the new test's
  KNOWN-DEFECT output.)
- D7. No `beforeunload` guard anywhere (item 5).

Tests print `KNOWN-DEFECT (present)` for D1, D2, D4, D5 (discard); when a page is fixed the line
flips to `KNOWN-DEFECT FIXED (update audit)`, which prompts removal of the entry here. D3 and D6
are not asserted.

## Negative tests (tools/negtest.ps1, each CAUGHT by an assertion `FAIL:` line, not a crash)

backup failure shown as "complete" (FAIL real POST 400); Ack-Delete always sent (FAIL no
deletions); reset failure shown as success (FAIL reset: HTTP 401..500); confirm result ignored
(FAIL declined confirm); kcUnit drops +32 (FAIL Fahrenheit); delta gets +32 (FAIL delta of +10 C);
live save drops server text (FAIL save 400); overwrite ignores checkbox (FAIL overwrite
without the checkbox). 8/8 CAUGHT, baseline passed.
